#include "mqtt_client.h"

#include <stdio.h>
#include <string.h>

#include "MQTTPacket.h"
#include "events.h"
#include "pal.h"

static const char *packet_name(int type)
{
    static const char *names[] = { "RESERVED", "CONNECT", "CONNACK", "PUBLISH", "PUBACK",
                                   "PUBREC", "PUBREL", "PUBCOMP", "SUBSCRIBE", "SUBACK",
                                   "UNSUBSCRIBE", "UNSUBACK", "PINGREQ", "PINGRESP",
                                   "DISCONNECT" };
    return (type >= 0 && type <= 14) ? names[type] : "UNKNOWN";
}

static void report(const char *dir_from, const char *dir_to, int type, const uint8_t *raw,
                   size_t len, int qos, double lat, const char *detail, const char *payload,
                   int payload_len, const char *extra)
{
    ev_t e = EV_INIT;
    e.proto = "MQTT";
    e.from = dir_from;
    e.to = dir_to;
    e.type = packet_name(type);
    e.raw = raw;
    e.raw_len = len;
    e.len = (int)len;
    e.qos = qos;
    e.lat_ms = lat;
    e.detail = detail;
    e.payload = payload;
    e.payload_len = payload_len;
    e.extra = extra;
    ev_emit(&e);
}

static uint16_t next_id(mqtt_t *m)
{
    if (++m->next_id == 0) {
        m->next_id = 1;
    }
    return m->next_id;
}

static int send_packet(mqtt_t *m, int len)
{
    if (len <= 0) {
        return MQTT_ERR;
    }
    if (link_send(m->link, m->tx, (size_t)len) != len) {
        m->connected = false;
        return MQTT_ERR;
    }
    m->last_tx_us = pal_now_us();
    return MQTT_OK;
}

/* Read exactly n bytes into dst before the deadline. */
static int read_exact(mqtt_t *m, uint8_t *dst, size_t n, uint64_t deadline_us)
{
    size_t got = 0;
    while (got < n) {
        uint64_t now = pal_now_us();
        int rc;
        if (now >= deadline_us) {
            return MQTT_TIMEOUT;
        }
        rc = link_recv(m->link, dst + got, n - got, (uint32_t)((deadline_us - now) / 1000 + 1));
        if (rc == LINK_TIMEOUT) {
            continue;
        }
        if (rc < 0) {
            m->connected = false;
            return MQTT_ERR;
        }
        got += (size_t)rc;
    }
    return MQTT_OK;
}

/* Returns packet type (> 0), MQTT_TIMEOUT if nothing started arriving, or
 * MQTT_ERR. A packet that starts arriving gets the full response timeout to
 * complete, otherwise the stream would be left mid-packet. */
static int read_packet(mqtt_t *m, uint32_t timeout_ms)
{
    uint64_t deadline;
    size_t hdr = 1, rem = 0, mult = 1;
    int rc = link_recv(m->link, m->rx, 1, timeout_ms);

    if (rc == LINK_TIMEOUT) {
        return MQTT_TIMEOUT;
    }
    if (rc < 0) {
        m->connected = false;
        return MQTT_ERR;
    }
    deadline = pal_now_us() + (uint64_t)m->cfg.timeout_ms * 1000ULL;
    for (;;) {
        if (hdr > 4 || read_exact(m, m->rx + hdr, 1, deadline) != MQTT_OK) {
            m->connected = false;
            return MQTT_ERR;
        }
        rem += (m->rx[hdr] & 0x7f) * mult;
        mult *= 128;
        if ((m->rx[hdr++] & 0x80) == 0) {
            break;
        }
    }
    if (hdr + rem > sizeof(m->rx)) {
        ev_log("MQTT", "incoming packet too large (%u bytes)", (unsigned)(hdr + rem));
        m->connected = false;
        return MQTT_ERR;
    }
    if (rem > 0 && read_exact(m, m->rx + hdr, rem, deadline) != MQTT_OK) {
        return MQTT_ERR;
    }
    m->rx_len = hdr + rem;
    return m->rx[0] >> 4;
}

static void handle_incoming_publish(mqtt_t *m)
{
    unsigned char dup, retained;
    unsigned short id;
    int qos, plen;
    unsigned char *payload;
    MQTTString topic;
    char t[160], detail[200], extra[32];

    if (MQTTDeserialize_publish(&dup, &qos, &retained, &id, &topic, &payload, &plen, m->rx,
                                (int)m->rx_len) != 1) {
        return;
    }
    snprintf(t, sizeof(t), "%.*s", topic.lenstring.len, topic.lenstring.data);
    snprintf(detail, sizeof(detail), "topic=%s", t);
    snprintf(extra, sizeof(extra), "\"id\":%u", (unsigned)id);
    {
        char text[512];
        snprintf(text, sizeof(text), "%.*s", plen > 500 ? 500 : plen, (const char *)payload);
        report("BROKER", "DEVICE", PUBLISH, m->rx, m->rx_len, qos, -1, detail, text, plen, extra);
    }
    if (qos == 1) {
        if (send_packet(m, MQTTSerialize_puback(m->tx, sizeof(m->tx), id)) == MQTT_OK) {
            report("DEVICE", "BROKER", PUBACK, m->tx, 4, 1, -1, NULL, NULL, -1, extra);
        }
    } else if (qos == 2) {
        int n = MQTTSerialize_ack(m->tx, sizeof(m->tx), PUBREC, 0, id);
        if (send_packet(m, n) == MQTT_OK) {
            report("DEVICE", "BROKER", PUBREC, m->tx, (size_t)n, 2, -1, NULL, NULL, -1, extra);
        }
    }
    if (m->cfg.on_message) {
        m->cfg.on_message(topic.lenstring.data, (size_t)topic.lenstring.len, payload,
                          (size_t)plen, m->cfg.user);
    }
}

/* Handle a packet that wasn't the one a caller was waiting for. */
static void dispatch(mqtt_t *m, int type)
{
    unsigned char ptype, dup;
    unsigned short id;

    switch (type) {
    case PUBLISH:
        handle_incoming_publish(m);
        break;
    case PUBREL: /* second half of an incoming QoS 2 */
        if (MQTTDeserialize_ack(&ptype, &dup, &id, m->rx, (int)m->rx_len) == 1) {
            int n;
            report("BROKER", "DEVICE", PUBREL, m->rx, m->rx_len, 2, -1, NULL, NULL, -1, NULL);
            n = MQTTSerialize_pubcomp(m->tx, sizeof(m->tx), id);
            if (send_packet(m, n) == MQTT_OK) {
                report("DEVICE", "BROKER", PUBCOMP, m->tx, (size_t)n, 2, -1, NULL, NULL, -1, NULL);
            }
        }
        break;
    case PINGRESP:
        report("BROKER", "DEVICE", PINGRESP, m->rx, m->rx_len, -1,
               m->ping_pending ? pal_ms_since(m->ping_sent_us) : -1, "keep-alive", NULL, -1, NULL);
        m->ping_pending = false;
        break;
    default:
        report("BROKER", "DEVICE", type, m->rx, m->rx_len, -1, -1, "unexpected", NULL, -1, NULL);
        break;
    }
}

/* Wait for a packet of `want` type (and packet id, if id != 0). */
static int wait_for(mqtt_t *m, int want, uint16_t want_id)
{
    uint64_t deadline = pal_now_us() + (uint64_t)m->cfg.timeout_ms * 1000ULL;

    for (;;) {
        uint64_t now = pal_now_us();
        int type;
        if (now >= deadline) {
            return MQTT_TIMEOUT;
        }
        type = read_packet(m, (uint32_t)((deadline - now) / 1000 + 1));
        if (type == MQTT_TIMEOUT) {
            continue;
        }
        if (type < 0) {
            return type;
        }
        if (type == want) {
            if (want_id != 0) {
                unsigned char t, dup;
                unsigned short id = 0;
                MQTTDeserialize_ack(&t, &dup, &id, m->rx, (int)m->rx_len);
                if (id != want_id) {
                    continue; /* stale ack from an earlier timed-out message */
                }
            }
            if (want == PINGRESP) {
                m->ping_pending = false;
            }
            return MQTT_OK;
        }
        dispatch(m, type);
    }
}

int mqtt_connect(mqtt_t *m, link_t *link, const mqtt_cfg_t *cfg, double *lat_ms)
{
    MQTTPacket_connectData data = MQTTPacket_connectData_initializer;
    unsigned char session_present, rc_code;
    uint64_t t0;
    char detail[96];
    int n, rc;

    memset(m, 0, sizeof(*m));
    m->link = link;
    m->cfg = *cfg;
    if (m->cfg.timeout_ms == 0) {
        m->cfg.timeout_ms = 10000;
    }
    data.MQTTVersion = 4; /* 3.1.1 */
    data.clientID.cstring = (char *)cfg->client_id;
    data.keepAliveInterval = cfg->keepalive_s;
    data.cleansession = 1;
    data.username.cstring = (char *)cfg->username;
    data.password.cstring = (char *)cfg->password;

    n = MQTTSerialize_connect(m->tx, sizeof(m->tx), &data);
    t0 = pal_now_us();
    if (send_packet(m, n) != MQTT_OK) {
        return MQTT_ERR;
    }
    snprintf(detail, sizeof(detail), "client_id=%s keepalive=%us clean=1", cfg->client_id,
             (unsigned)cfg->keepalive_s);
    report("DEVICE", "BROKER", CONNECT, m->tx, (size_t)n, -1, -1, detail, NULL, -1, NULL);

    rc = wait_for(m, CONNACK, 0);
    if (rc != MQTT_OK) {
        ev_log("MQTT", "no CONNACK (%s%s%s)", rc == MQTT_TIMEOUT ? "timeout" : "connection lost",
               link->err[0] ? ": " : "", link->err);
        return rc;
    }
    *lat_ms = pal_ms_since(t0);
    if (MQTTDeserialize_connack(&session_present, &rc_code, m->rx, (int)m->rx_len) != 1) {
        return MQTT_ERR;
    }
    snprintf(detail, sizeof(detail), "return_code=%u%s", (unsigned)rc_code,
             rc_code == 0 ? " (accepted)" : " (refused)");
    report("BROKER", "DEVICE", CONNACK, m->rx, m->rx_len, -1, *lat_ms, detail, NULL, -1, NULL);
    if (rc_code != 0) {
        return MQTT_REFUSED;
    }
    m->connected = true;
    return MQTT_OK;
}

int mqtt_publish(mqtt_t *m, const char *topic, const uint8_t *payload, size_t len, int qos,
                 bool probe, const char *detail, mqtt_pub_result_t *res)
{
    MQTTString t = MQTTString_initializer;
    uint64_t t0;
    uint64_t tx0 = m->link->st.tx_bytes, rx0 = m->link->st.rx_bytes;
    uint16_t id = qos ? next_id(m) : 0;
    char extra[64];
    int n, rc;

    memset(res, 0, sizeof(*res));
    res->lat_ms = -1;
    if (!m->connected) {
        return MQTT_ERR;
    }
    t.cstring = (char *)topic;
    n = MQTTSerialize_publish(m->tx, sizeof(m->tx), 0, qos, 0, id, t, (unsigned char *)payload,
                              (int)len);
    if (n <= 0) {
        ev_log("MQTT", "payload too large for the %u-byte packet buffer", (unsigned)sizeof(m->tx));
        return MQTT_ERR;
    }
    t0 = pal_now_us();
    if (send_packet(m, n) != MQTT_OK) {
        return MQTT_ERR;
    }
    res->packet_id = id;
    snprintf(extra, sizeof(extra), "\"id\":%u,\"topic\":\"%s\"", (unsigned)id, topic);
    report("DEVICE", "BROKER", PUBLISH, m->tx, (size_t)n, qos, -1, detail, (const char *)payload,
           (int)len, extra);

    if (qos == 0) {
        if (probe) {
            n = MQTTSerialize_pingreq(m->tx, sizeof(m->tx));
            if (send_packet(m, n) != MQTT_OK) {
                return MQTT_ERR;
            }
            report("DEVICE", "BROKER", PINGREQ, m->tx, (size_t)n, -1, -1,
                   "latency probe (QoS 0 has no ack)", NULL, -1, NULL);
            m->ping_pending = true;
            m->ping_sent_us = pal_now_us();
            rc = wait_for(m, PINGRESP, 0);
            if (rc != MQTT_OK) {
                return rc;
            }
            res->lat_ms = pal_ms_since(t0);
            report("BROKER", "DEVICE", PINGRESP, m->rx, m->rx_len, -1, res->lat_ms,
                   "broker has read the PUBLISH", NULL, -1, NULL);
        }
    } else if (qos == 1) {
        rc = wait_for(m, PUBACK, id);
        if (rc != MQTT_OK) {
            return rc;
        }
        res->lat_ms = res->step_ms[0] = pal_ms_since(t0);
        report("BROKER", "DEVICE", PUBACK, m->rx, m->rx_len, 1, res->lat_ms, NULL, NULL, -1, extra);
    } else {
        rc = wait_for(m, PUBREC, id);
        if (rc != MQTT_OK) {
            return rc;
        }
        res->step_ms[0] = pal_ms_since(t0);
        report("BROKER", "DEVICE", PUBREC, m->rx, m->rx_len, 2, res->step_ms[0],
               "step 1/2: broker stored the message", NULL, -1, extra);
        n = MQTTSerialize_pubrel(m->tx, sizeof(m->tx), 0, id);
        if (send_packet(m, n) != MQTT_OK) {
            return MQTT_ERR;
        }
        report("DEVICE", "BROKER", PUBREL, m->tx, (size_t)n, 2, -1, "release", NULL, -1, extra);
        rc = wait_for(m, PUBCOMP, id);
        if (rc != MQTT_OK) {
            return rc;
        }
        res->lat_ms = res->step_ms[1] = pal_ms_since(t0);
        report("BROKER", "DEVICE", PUBCOMP, m->rx, m->rx_len, 2, res->lat_ms,
               "step 2/2: exactly-once delivery complete", NULL, -1, extra);
    }
    res->tx_bytes = (uint32_t)(m->link->st.tx_bytes - tx0);
    res->rx_bytes = (uint32_t)(m->link->st.rx_bytes - rx0);
    return MQTT_OK;
}

int mqtt_subscribe(mqtt_t *m, const char *topic, int qos, double *lat_ms)
{
    MQTTString t = MQTTString_initializer;
    unsigned short ack_id;
    int count = 0, granted = 0x80, n, rc;
    uint16_t id = next_id(m);
    uint64_t t0;
    char detail[192];

    t.cstring = (char *)topic;
    n = MQTTSerialize_subscribe(m->tx, sizeof(m->tx), 0, id, 1, &t, &qos);
    t0 = pal_now_us();
    if (send_packet(m, n) != MQTT_OK) {
        return MQTT_ERR;
    }
    snprintf(detail, sizeof(detail), "topic=%s", topic);
    report("DEVICE", "BROKER", SUBSCRIBE, m->tx, (size_t)n, qos, -1, detail, NULL, -1, NULL);
    rc = wait_for(m, SUBACK, 0);
    if (rc != MQTT_OK) {
        return rc;
    }
    *lat_ms = pal_ms_since(t0);
    MQTTDeserialize_suback(&ack_id, 1, &count, &granted, m->rx, (int)m->rx_len);
    snprintf(detail, sizeof(detail), "granted_qos=%d%s", granted, granted == 0x80 ? " (failure)" : "");
    report("BROKER", "DEVICE", SUBACK, m->rx, m->rx_len, -1, *lat_ms, detail, NULL, -1, NULL);
    return granted == 0x80 ? MQTT_REFUSED : MQTT_OK;
}

int mqtt_poll(mqtt_t *m, uint32_t timeout_ms)
{
    uint64_t now = pal_now_us();
    uint64_t ka_us = (uint64_t)m->cfg.keepalive_s * 1000000ULL;
    int type;

    if (!m->connected) {
        return MQTT_ERR;
    }
    if (ka_us != 0) {
        if (m->ping_pending && now - m->ping_sent_us > (uint64_t)m->cfg.timeout_ms * 1000ULL) {
            ev_log("MQTT", "PINGRESP timeout - connection considered dead");
            m->connected = false;
            return MQTT_ERR;
        }
        if (!m->ping_pending && now - m->last_tx_us >= ka_us * 3 / 4) {
            int n = MQTTSerialize_pingreq(m->tx, sizeof(m->tx));
            if (send_packet(m, n) != MQTT_OK) {
                return MQTT_ERR;
            }
            report("DEVICE", "BROKER", PINGREQ, m->tx, (size_t)n, -1, -1, "keep-alive", NULL, -1, NULL);
            m->ping_pending = true;
            m->ping_sent_us = now;
        }
    }
    while ((type = read_packet(m, timeout_ms)) > 0) {
        dispatch(m, type);
        timeout_ms = 0;
    }
    return type == MQTT_TIMEOUT ? MQTT_OK : type;
}

void mqtt_disconnect(mqtt_t *m)
{
    if (m->connected) {
        int n = MQTTSerialize_disconnect(m->tx, sizeof(m->tx));
        if (send_packet(m, n) == MQTT_OK) {
            report("DEVICE", "BROKER", DISCONNECT, m->tx, (size_t)n, -1, -1, NULL, NULL, -1, NULL);
        }
    }
    m->connected = false;
}
