#include "protocol/mqtt_client.h"

#include <string.h>

#include "common/fw_config.h"
#include "common/fw_log.h"
#include "hal/timer.h"

#define TAG "mqtt"

#if FW_HAVE_PAHO

#include "MQTTPacket.h"

static struct {
    mqtt_config_t cfg;
    fw_socket_t sock;
    tls_session_t tls;
    fw_transport_t tr;
    bool connected;
    uint16_t next_id;
    uint32_t last_tx;
    bool ping_outstanding;
    uint32_t ping_sent;
    /* packet reader state (Paho's getfn has no context argument) */
    hal_timer_t deadline;
    bool got_header;
    int read_err;
    uint8_t tx[FW_MQTT_BUF_SIZE];
    uint8_t rx[FW_MQTT_BUF_SIZE];
} m;

static void drop(void)
{
    if (fw_transport_is_open(&m.tr)) {
        fw_transport_close(&m.tr);
    }
    m.connected = false;
}

static uint16_t next_packet_id(void)
{
    if (++m.next_id == 0) {
        m.next_id = 1;
    }
    return m.next_id;
}

/* Paho getfn: must return exactly len bytes, anything else is a failure. */
static int read_exact(unsigned char *buf, int len)
{
    int got = 0;

    while (got < len) {
        uint32_t remaining = hal_timer_remaining(&m.deadline);
        int rc;

        if (remaining == 0) {
            m.read_err = FW_ERR_TIMEOUT;
            return -1;
        }
        rc = fw_transport_recv(&m.tr, buf + got, (size_t)(len - got), remaining);
        if (rc < 0) {
            m.read_err = rc;
            return -1;
        }
        got += rc;
        if (!m.got_header) {
            /* Packet started: the rest must arrive within the network timeout,
             * independent of how short the caller's poll window was. */
            m.got_header = true;
            hal_timer_start(&m.deadline, m.cfg.timeout_ms);
        }
    }
    return got;
}

/* Returns packet type (> 0), FW_ERR_TIMEOUT if nothing arrived, or an error
 * that leaves the stream unusable. */
static int read_packet(uint32_t timeout_ms)
{
    int type;

    m.read_err = FW_OK;
    m.got_header = false;
    hal_timer_start(&m.deadline, timeout_ms);

    type = MQTTPacket_read(m.rx, (int)sizeof(m.rx), read_exact);
    if (type > 0) {
        return type;
    }
    if (m.read_err == FW_ERR_TIMEOUT && !m.got_header) {
        return FW_ERR_TIMEOUT;
    }
    if (m.read_err == FW_OK) {
        LOG_E(TAG, "malformed or oversized packet (buffer %u)", (unsigned)sizeof(m.rx));
        return FW_ERR_IO;
    }
    return m.read_err;
}

static int send_packet(int len)
{
    int rc;

    if (len <= 0) {
        return FW_ERR_NOMEM; /* serialiser ran out of buffer */
    }
    rc = fw_transport_send(&m.tr, m.tx, (size_t)len);
    if (rc < 0) {
        LOG_W(TAG, "send failed (%d)", rc);
        drop();
        return rc;
    }
    m.last_tx = hal_millis();
    return FW_OK;
}

static void handle_publish(void)
{
    unsigned char dup, retained;
    unsigned short id;
    int qos, payload_len;
    unsigned char *payload;
    MQTTString topic;

    if (MQTTDeserialize_publish(&dup, &qos, &retained, &id, &topic, &payload, &payload_len,
                                m.rx, (int)sizeof(m.rx)) != 1) {
        LOG_W(TAG, "bad PUBLISH");
        return;
    }
    if (qos == 1) {
        (void)send_packet(MQTTSerialize_puback(m.tx, (int)sizeof(m.tx), id));
    }
    if (m.cfg.on_message != NULL) {
        m.cfg.on_message(topic.lenstring.data, (size_t)topic.lenstring.len,
                         payload, (size_t)payload_len, m.cfg.user);
    }
}

static void handle_packet(int type)
{
    switch (type) {
    case PUBLISH:
        handle_publish();
        break;
    case PINGRESP:
        m.ping_outstanding = false;
        break;
    default:
        LOG_D(TAG, "ignored packet type %d", type);
        break;
    }
}

/* Read until a packet of `want` arrives; others are dispatched normally. */
static int wait_for(int want)
{
    hal_timer_t overall;

    hal_timer_start(&overall, m.cfg.timeout_ms);
    for (;;) {
        uint32_t remaining = hal_timer_remaining(&overall);
        int type;

        if (remaining == 0) {
            return FW_ERR_TIMEOUT;
        }
        type = read_packet(remaining);
        if (type == want) {
            return FW_OK;
        }
        if (type < 0) {
            return type;
        }
        handle_packet(type);
    }
}

int mqtt_client_connect(const mqtt_config_t *cfg)
{
    MQTTPacket_connectData data = MQTTPacket_connectData_initializer;
    unsigned char session_present, connack_rc;
    int rc;

    if (cfg == NULL || cfg->host == NULL || cfg->client_id == NULL) {
        return FW_ERR_PARAM;
    }
    mqtt_client_disconnect();
    m.cfg = *cfg;
    if (m.cfg.timeout_ms == 0) {
        m.cfg.timeout_ms = FW_NET_TIMEOUT_MS;
    }

    if (cfg->use_tls) {
        rc = tls_connect(&m.tls, cfg->host, cfg->port, &cfg->tls);
        if (rc != FW_OK) {
            return rc;
        }
        tls_transport(&m.tls, &m.tr);
    } else {
        rc = fw_socket_connect(&m.sock, PORT_SOCK_TCP, cfg->host, cfg->port);
        if (rc != FW_OK) {
            return rc;
        }
        network_socket_transport(&m.sock, &m.tr);
    }

    data.MQTTVersion = 4; /* 3.1.1 */
    data.clientID.cstring = (char *)cfg->client_id;
    data.keepAliveInterval = cfg->keepalive_s;
    data.cleansession = 1;
    data.username.cstring = (char *)cfg->username;
    data.password.cstring = (char *)cfg->password;

    rc = send_packet(MQTTSerialize_connect(m.tx, (int)sizeof(m.tx), &data));
    if (rc == FW_OK) {
        rc = wait_for(CONNACK);
    }
    if (rc != FW_OK) {
        LOG_W(TAG, "no CONNACK (%d)", rc);
        drop();
        return rc;
    }
    if (MQTTDeserialize_connack(&session_present, &connack_rc, m.rx, (int)sizeof(m.rx)) != 1 ||
        connack_rc != 0) {
        LOG_E(TAG, "connection refused (code %u)", (unsigned)connack_rc);
        drop();
        return FW_ERR_SECURITY;
    }
    m.connected = true;
    m.ping_outstanding = false;
    LOG_I(TAG, "connected to %s:%u as '%s'", cfg->host, (unsigned)cfg->port, cfg->client_id);
    return FW_OK;
}

int mqtt_client_publish(const char *topic, const uint8_t *payload, size_t len, int qos)
{
    MQTTString t = MQTTString_initializer;
    unsigned char type, dup;
    unsigned short ack_id, id;
    int rc;

    if (topic == NULL || (payload == NULL && len > 0) || qos < 0 || qos > 1) {
        return FW_ERR_PARAM;
    }
    if (!m.connected) {
        return FW_ERR_STATE;
    }
    t.cstring = (char *)topic;
    id = qos ? next_packet_id() : 0;

    rc = send_packet(MQTTSerialize_publish(m.tx, (int)sizeof(m.tx), 0, qos, 0, id, t,
                                           (unsigned char *)payload, (int)len));
    if (rc != FW_OK || qos == 0) {
        return rc;
    }
    rc = wait_for(PUBACK);
    if (rc != FW_OK) {
        drop();
        return rc;
    }
    if (MQTTDeserialize_ack(&type, &dup, &ack_id, m.rx, (int)sizeof(m.rx)) != 1 || ack_id != id) {
        LOG_W(TAG, "PUBACK id mismatch (%u != %u)", (unsigned)ack_id, (unsigned)id);
    }
    return FW_OK;
}

int mqtt_client_subscribe(const char *topic, int qos)
{
    MQTTString t = MQTTString_initializer;
    unsigned short id, ack_id;
    int count = 0, granted = 0x80;
    int rc;

    if (topic == NULL || qos < 0 || qos > 1) {
        return FW_ERR_PARAM;
    }
    if (!m.connected) {
        return FW_ERR_STATE;
    }
    t.cstring = (char *)topic;
    id = next_packet_id();

    rc = send_packet(MQTTSerialize_subscribe(m.tx, (int)sizeof(m.tx), 0, id, 1, &t, &qos));
    if (rc == FW_OK) {
        rc = wait_for(SUBACK);
    }
    if (rc != FW_OK) {
        drop();
        return rc;
    }
    if (MQTTDeserialize_suback(&ack_id, 1, &count, &granted, m.rx, (int)sizeof(m.rx)) != 1 ||
        granted == 0x80) {
        LOG_W(TAG, "subscribe '%s' rejected", topic);
        return FW_ERR;
    }
    LOG_I(TAG, "subscribed '%s' (qos %d)", topic, granted);
    return FW_OK;
}

int mqtt_client_poll(uint32_t timeout_ms)
{
    uint32_t now, keepalive_ms;
    int type;

    if (!m.connected) {
        return FW_ERR_STATE;
    }
    now = hal_millis();
    keepalive_ms = (uint32_t)m.cfg.keepalive_s * 1000u;
    if (keepalive_ms != 0) {
        if (m.ping_outstanding && (uint32_t)(now - m.ping_sent) > m.cfg.timeout_ms) {
            LOG_W(TAG, "PINGRESP timeout");
            drop();
            return FW_ERR_TIMEOUT;
        }
        if (!m.ping_outstanding && (uint32_t)(now - m.last_tx) >= keepalive_ms * 3u / 4u) {
            int rc = send_packet(MQTTSerialize_pingreq(m.tx, (int)sizeof(m.tx)));
            if (rc != FW_OK) {
                return rc;
            }
            m.ping_outstanding = true;
            m.ping_sent = now;
        }
    }

    /* First packet may take up to timeout_ms; then drain whatever is queued. */
    while ((type = read_packet(timeout_ms)) > 0) {
        handle_packet(type);
        timeout_ms = 0;
    }
    if (type != FW_ERR_TIMEOUT) {
        LOG_W(TAG, "connection lost (%d)", type);
        drop();
        return type;
    }
    return FW_OK;
}

bool mqtt_client_is_connected(void)
{
    return m.connected;
}

void mqtt_client_disconnect(void)
{
    if (m.connected) {
        (void)send_packet(MQTTSerialize_disconnect(m.tx, (int)sizeof(m.tx)));
        LOG_I(TAG, "disconnected");
    }
    drop();
}

#else /* !FW_HAVE_PAHO */

int mqtt_client_connect(const mqtt_config_t *cfg)
{
    FW_UNUSED(cfg);
    LOG_E(TAG, "built without Paho (FW_USE_PAHO=OFF)");
    return FW_ERR_UNSUPPORTED;
}

int mqtt_client_publish(const char *topic, const uint8_t *payload, size_t len, int qos)
{
    FW_UNUSED(topic); FW_UNUSED(payload); FW_UNUSED(len); FW_UNUSED(qos);
    return FW_ERR_UNSUPPORTED;
}

int mqtt_client_subscribe(const char *topic, int qos)
{
    FW_UNUSED(topic); FW_UNUSED(qos);
    return FW_ERR_UNSUPPORTED;
}

int mqtt_client_poll(uint32_t timeout_ms)
{
    FW_UNUSED(timeout_ms);
    return FW_ERR_UNSUPPORTED;
}

bool mqtt_client_is_connected(void)
{
    return false;
}

void mqtt_client_disconnect(void)
{
}

#endif /* FW_HAVE_PAHO */
