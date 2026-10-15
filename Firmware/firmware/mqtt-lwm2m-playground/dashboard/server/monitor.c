/*
 * Server-side views for the dashboard:
 *
 *  - MQTT: the lab keeps its own client connection to the broker, subscribes
 *    to the broker's $SYS statistics and to playground/+/telemetry, and so
 *    sees what the broker actually delivers.
 *  - LwM2M: the lab follows Leshan's event stream (GET /api/event, Server-Sent
 *    Events) for registrations and notifications as Leshan received them.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "MQTTPacket.h"
#include "events.h"
#include "lab.h"
#include "pal.h"
#include "upstream.h"

/* On a shared public server, only show our own devices. */
static bool ours(const char *text)
{
    return !L.public_mode || strstr(text, L.id_prefix) != NULL;
}

/* ---- MQTT monitor -------------------------------------------------------- */

static struct {
    pal_sock_t sock;
    bool up;
    lab_mutex_t tx;
    uint16_t next_id;
    uint8_t rx[16384];
} mq = { PAL_SOCK_INVALID };

static int read_n(pal_sock_t s, uint8_t *dst, size_t n, uint32_t timeout_ms)
{
    size_t got = 0;
    while (got < n) {
        int r = pal_sock_recv(s, dst + got, n - got, timeout_ms);
        if (r <= 0) {
            return -1;
        }
        got += (size_t)r;
    }
    return 0;
}

/* Returns packet type, 0 on idle timeout, -1 on error. */
static int mq_read(size_t *len, uint32_t idle_ms)
{
    size_t hdr = 1, rem = 0, mult = 1;
    int r = pal_sock_recv(mq.sock, mq.rx, 1, idle_ms);
    if (r == 0) {
        return 0;
    }
    if (r < 0) {
        return -1;
    }
    do {
        if (hdr > 4 || read_n(mq.sock, mq.rx + hdr, 1, 5000) != 0) {
            return -1;
        }
        rem += (mq.rx[hdr] & 0x7f) * mult;
        mult *= 128;
    } while (mq.rx[hdr++] & 0x80);
    if (hdr + rem > sizeof(mq.rx)) {
        /* too big for us: skip it */
        uint8_t junk[512];
        while (rem > 0) {
            size_t k = rem > sizeof(junk) ? sizeof(junk) : rem;
            if (read_n(mq.sock, junk, k, 5000) != 0) return -1;
            rem -= k;
        }
        *len = 0;
        return 0;
    }
    if (rem && read_n(mq.sock, mq.rx + hdr, rem, 5000) != 0) {
        return -1;
    }
    *len = hdr + rem;
    return mq.rx[0] >> 4;
}

static int mq_send(const uint8_t *buf, int len)
{
    int rc;
    if (len <= 0) {
        return -1;
    }
    mutex_lock(&mq.tx);
    rc = mq.up ? pal_sock_send_all(mq.sock, buf, (size_t)len) : -1;
    mutex_unlock(&mq.tx);
    return rc;
}

static void on_publish(size_t len)
{
    unsigned char dup, retained;
    unsigned short id;
    int qos, plen;
    unsigned char *payload;
    MQTTString topic;
    char t[160], p[1024];

    if (MQTTDeserialize_publish(&dup, &qos, &retained, &id, &topic, &payload, &plen, mq.rx, (int)len) != 1) {
        return;
    }
    snprintf(t, sizeof(t), "%.*s", topic.lenstring.len, topic.lenstring.data);
    snprintf(p, sizeof(p), "%.*s", plen > 1000 ? 1000 : plen, (const char *)payload);
    if (qos == 1) {
        uint8_t ack[8];
        mq_send(ack, MQTTSerialize_puback(ack, sizeof(ack), id));
    }

    mutex_lock(&L.lock);
    if (strncmp(t, "$SYS/", 5) == 0) {
        long v = atol(p);
        if (strcmp(t, "$SYS/broker/version") == 0) snprintf(L.broker_version, sizeof(L.broker_version), "%s", p);
        else if (strcmp(t, "$SYS/broker/clients/connected") == 0) L.sys_clients = v;
        else if (strcmp(t, "$SYS/broker/messages/received") == 0) L.sys_msgs_rx = v;
        else if (strcmp(t, "$SYS/broker/messages/sent") == 0) L.sys_msgs_tx = v;
        else if (strcmp(t, "$SYS/broker/publish/messages/received") == 0) L.sys_pub_rx = v;
        else if (strcmp(t, "$SYS/broker/publish/messages/sent") == 0) L.sys_pub_tx = v;
        mutex_unlock(&L.lock);
        return;
    }
    L.sub_received++;
    snprintf(L.last_topic, sizeof(L.last_topic), "%s", t);
    snprintf(L.last_payload, sizeof(L.last_payload), "%s", p);
    L.last_t = pal_wall_ms();
    mutex_unlock(&L.lock);

    {
        char esc_t[400], esc_p[2100], out[2800];
        json_escape(esc_t, sizeof(esc_t), t, sizeof(t));
        json_escape(esc_p, sizeof(esc_p), p, sizeof(p));
        snprintf(out, sizeof(out),
                 "{\"k\":\"broker\",\"t\":%llu,\"topic\":\"%s\",\"payload\":\"%s\",\"len\":%d,\"qos\":%d}",
                 (unsigned long long)pal_wall_ms(), esc_t, esc_p, plen, qos);
        lab_broadcast(out);
    }
}

static void mqtt_monitor(void *arg)
{
    static char telemetry[96];
    static const char *topics[] = {
        "$SYS/broker/version", "$SYS/broker/clients/connected", "$SYS/broker/messages/received",
        "$SYS/broker/messages/sent", "$SYS/broker/publish/messages/received",
        "$SYS/broker/publish/messages/sent", telemetry,
    };
    (void)arg;
    /* A public broker's $SYS counters are about everyone (and often not
     * subscribable): there, watch only our own telemetry. */
    const size_t first = L.public_mode ? sizeof(topics) / sizeof(topics[0]) - 1 : 0;
    snprintf(telemetry, sizeof(telemetry), "%s/+/telemetry", L.topic);
    for (;;) {
        MQTTPacket_connectData cd = MQTTPacket_connectData_initializer;
        uint8_t buf[512];
        size_t len;
        uint64_t last_ping;
        int n, type;

        if (pal_sock_connect(&mq.sock, false, L.mqtt_host, L.mqtt_port) != 0) {
            mutex_lock(&L.lock);
            L.broker_online = false;
            mutex_unlock(&L.lock);
            pal_sleep_ms(2000);
            continue;
        }
        cd.MQTTVersion = 4;
        cd.clientID.cstring = "playground-lab-monitor";
        cd.keepAliveInterval = 30;
        cd.cleansession = 1;
        n = MQTTSerialize_connect(buf, sizeof(buf), &cd);
        if (pal_sock_send_all(mq.sock, buf, (size_t)n) != 0 || mq_read(&len, 5000) != CONNACK) {
            pal_sock_close(mq.sock);
            pal_sleep_ms(2000);
            continue;
        }
        mutex_lock(&mq.tx);
        mq.up = true;
        mutex_unlock(&mq.tx);
        for (size_t i = first; i < sizeof(topics) / sizeof(topics[0]); i++) {
            MQTTString t = MQTTString_initializer;
            int q = (i == sizeof(topics) / sizeof(topics[0]) - 1) ? 1 : 0;
            t.cstring = (char *)topics[i];
            n = MQTTSerialize_subscribe(buf, sizeof(buf), 0, ++mq.next_id, 1, &t, &q);
            mq_send(buf, n);
        }
        mutex_lock(&L.lock);
        L.broker_online = true;
        mutex_unlock(&L.lock);
        printf("[lab] connected to MQTT broker %s:%u\n", L.mqtt_host, (unsigned)L.mqtt_port);
        last_ping = pal_now_us();

        for (;;) {
            type = mq_read(&len, 1000);
            if (type < 0) {
                break;
            }
            if (type == PUBLISH) {
                on_publish(len);
            }
            if (pal_now_us() - last_ping > 20000000ULL) {
                n = MQTTSerialize_pingreq(buf, sizeof(buf));
                if (mq_send(buf, n) != 0) {
                    break;
                }
                last_ping = pal_now_us();
            }
        }
        mutex_lock(&mq.tx);
        mq.up = false;
        pal_sock_close(mq.sock);
        mutex_unlock(&mq.tx);
        mutex_lock(&L.lock);
        L.broker_online = false;
        mutex_unlock(&L.lock);
        printf("[lab] lost MQTT broker connection, retrying\n");
        pal_sleep_ms(2000);
    }
}

int monitor_publish(const char *topic, const char *payload, int qos)
{
    MQTTString t = MQTTString_initializer;
    uint8_t buf[2048];
    int n;
    t.cstring = (char *)topic;
    n = MQTTSerialize_publish(buf, sizeof(buf), 0, qos, 0, qos ? ++mq.next_id : 0, t,
                              (unsigned char *)payload, (int)strlen(payload));
    return mq_send(buf, n);
}

/* ---- Leshan monitor ------------------------------------------------------ */

/* HTTP/1.0 so the reply is never chunked; read until the server closes. */
upstream_t *leshan_connect(void)
{
    return upstream_open(L.leshan_host, L.leshan_port, L.leshan_tls, L.leshan_ca);
}

static int leshan_get(const char *path, char *out, size_t cap)
{
    upstream_t *s;
    char req[256];
    size_t got = 0;
    char *body;
    int status = 0;

    if ((s = leshan_connect()) == NULL) {
        return -1;
    }
    snprintf(req, sizeof(req), "GET %s HTTP/1.0\r\nHost: %s\r\nAccept: application/json\r\n\r\n", path,
             L.leshan_host);
    upstream_send_all(s, req, strlen(req));
    for (;;) {
        int n;
        if (got + 1 >= cap) break;
        n = upstream_recv(s, out + got, cap - 1 - got, 5000);
        if (n <= 0) break;
        got += (size_t)n;
    }
    upstream_close(s);
    out[got] = '\0';
    sscanf(out, "HTTP/%*s %d", &status);
    body = strstr(out, "\r\n\r\n");
    if (body) {
        memmove(out, body + 4, strlen(body + 4) + 1);
    }
    return status;
}

static void leshan_poll(void *arg)
{
    static char buf[1 << 20]; /* the public sandbox lists everyone's clients */
    (void)arg;
    for (;;) {
        int status = leshan_get("/api/clients", buf, sizeof(buf));
        mutex_lock(&L.lock);
        L.leshan_online = status == 200;
        if (status == 200) {
            char *p = buf;
            size_t o = 0;
            L.leshan_clients = 0;
            L.leshan_endpoints[0] = '\0';
            while ((p = strstr(p, "\"endpoint\":\"")) != NULL) {
                char *e;
                p += 12;
                e = strchr(p, '"');
                if (e == NULL) break;
                if (L.public_mode && strncmp(p, L.id_prefix, strlen(L.id_prefix)) != 0) {
                    p = e;
                    continue;
                }
                L.leshan_clients++;
                if (o + (size_t)(e - p) + 2 < sizeof(L.leshan_endpoints)) {
                    o += (size_t)snprintf(L.leshan_endpoints + o, sizeof(L.leshan_endpoints) - o, "%s%.*s",
                                          o ? "," : "", (int)(e - p), p);
                }
                p = e;
            }
        }
        mutex_unlock(&L.lock);
        pal_sleep_ms(2000);
    }
}

static void remember_value(const char *data)
{
    const char *r = strstr(data, "\"res\":\"");
    const char *v = strstr(data, "\"value\":");
    char path[48] = "", value[64] = "";
    int slot = -1;

    if (r) sscanf(r + 7, "%47[^\"]", path);
    if (v) {
        v += 8;
        if (*v == '"') sscanf(v + 1, "%63[^\"]", value);
        else sscanf(v, "%63[^,}]", value);
    }
    if (!path[0]) return;
    mutex_lock(&L.lock);
    L.leshan_notifications++;
    for (int i = 0; i < LAB_MAX_RES; i++) {
        if (strcmp(L.res[i].path, path) == 0 || (slot < 0 && L.res[i].path[0] == '\0')) {
            slot = i;
            if (strcmp(L.res[i].path, path) == 0) break;
        }
    }
    if (slot >= 0) {
        snprintf(L.res[slot].path, sizeof(L.res[slot].path), "%s", path);
        snprintf(L.res[slot].value, sizeof(L.res[slot].value), "%s", value);
        L.res[slot].t = pal_wall_ms();
    }
    mutex_unlock(&L.lock);
}

static void leshan_events(void *arg)
{
    static char line[16384];
    (void)arg;
    for (;;) {
        upstream_t *s;
        char req[256], event[32] = "", chunk[4096];
        size_t ll = 0;
        bool in_body = false;

        if ((s = leshan_connect()) == NULL) {
            pal_sleep_ms(3000);
            continue;
        }
        snprintf(req, sizeof(req), "GET /api/event HTTP/1.0\r\nHost: %s\r\nAccept: text/event-stream\r\n\r\n",
                 L.leshan_host);
        upstream_send_all(s, req, strlen(req));
        printf("[lab] following Leshan events at %s://%s:%u/api/event\n", L.leshan_tls ? "https" : "http",
               L.leshan_host, (unsigned)L.leshan_port);
        for (;;) {
            int n = upstream_recv(s, chunk, sizeof(chunk), 60000);
            if (n == 0) continue;
            if (n < 0) break;
            for (int i = 0; i < n; i++) {
            char ch = chunk[i];
            if (ch == '\r') continue;
            if (ch != '\n') {
                if (ll + 1 < sizeof(line)) line[ll++] = ch;
                continue;
            }
            line[ll] = '\0';
            ll = 0;
            if (!in_body) {
                if (line[0] == '\0') in_body = true; /* end of HTTP headers */
                continue;
            }
            if (strncmp(line, "event:", 6) == 0) {
                snprintf(event, sizeof(event), "%s", line + 6 + (line[6] == ' '));
            } else if (strncmp(line, "data:", 5) == 0 && event[0] && ours(line)) {
                const char *data = line + 5 + (line[5] == ' ');
                static char out[17000];
                if (strcmp(event, "NOTIFICATION") == 0) {
                    remember_value(data);
                } else if (strcmp(event, "REGISTRATION") == 0) {
                    mutex_lock(&L.lock);
                    L.leshan_registrations++;
                    mutex_unlock(&L.lock);
                }
                if (strcmp(event, "COAPLOG") != 0) {
                    snprintf(out, sizeof(out), "{\"k\":\"leshan\",\"t\":%llu,\"event\":\"%s\",\"data\":%s}",
                             (unsigned long long)pal_wall_ms(), event, data[0] == '{' ? data : "null");
                    lab_broadcast(out);
                }
            } else if (line[0] == '\0') {
                event[0] = '\0';
            }
            }
        }
        upstream_close(s);
        pal_sleep_ms(3000);
    }
}

/* ---- public -------------------------------------------------------------- */

void monitor_start(void)
{
    mutex_init(&mq.tx);
    thread_start(mqtt_monitor, NULL);
    thread_start(leshan_poll, NULL);
    thread_start(leshan_events, NULL);
}

void monitor_reset_counters(void)
{
    mutex_lock(&L.lock);
    L.sub_received = 0;
    L.leshan_notifications = 0;
    L.leshan_registrations = 0;
    memset(L.res, 0, sizeof(L.res));
    mutex_unlock(&L.lock);
}

void monitor_state_json(char *out, size_t cap)
{
    char esc_p[2100], esc_v[200];
    size_t o;
    mutex_lock(&L.lock);
    json_escape(esc_p, sizeof(esc_p), L.last_payload, sizeof(L.last_payload));
    json_escape(esc_v, sizeof(esc_v), L.broker_version, sizeof(L.broker_version));
    o = (size_t)snprintf(out, cap,
        "\"mqtt\":{\"online\":%s,\"host\":\"%s\",\"port\":%u,\"tls_port\":%u,\"version\":\"%s\","
        "\"clients_connected\":%ld,\"msgs_received\":%ld,\"msgs_sent\":%ld,\"publish_received\":%ld,"
        "\"publish_sent\":%ld,\"lab_received\":%u,\"last_topic\":\"%s\",\"last_payload\":\"%s\",\"last_t\":%llu},"
        "\"lwm2m\":{\"online\":%s,\"host\":\"%s\",\"web_port\":%u,\"coap_port\":%u,\"coaps_port\":%u,"
        "\"clients\":%d,\"endpoints\":\"%s\",\"notifications\":%u,\"registrations\":%u,\"resources\":[",
        L.broker_online ? "true" : "false", L.mqtt_host, (unsigned)L.mqtt_port, (unsigned)L.mqtts_port,
        esc_v, L.sys_clients, L.sys_msgs_rx, L.sys_msgs_tx, L.sys_pub_rx, L.sys_pub_tx, L.sub_received,
        L.last_topic, esc_p, (unsigned long long)L.last_t, L.leshan_online ? "true" : "false",
        L.leshan_host, (unsigned)L.leshan_port, (unsigned)L.coap_port, (unsigned)L.coaps_port,
        L.leshan_clients, L.leshan_endpoints, L.leshan_notifications, L.leshan_registrations);
    for (int i = 0, first = 1; i < LAB_MAX_RES && o < cap; i++) {
        if (L.res[i].path[0]) {
            json_escape(esc_v, sizeof(esc_v), L.res[i].value, sizeof(L.res[i].value));
            o += (size_t)snprintf(out + o, cap - o, "%s{\"path\":\"%s\",\"value\":\"%s\",\"t\":%llu}",
                                  first ? "" : ",", L.res[i].path, esc_v, (unsigned long long)L.res[i].t);
            first = 0;
        }
    }
    if (o < cap) snprintf(out + o, cap - o, "]}");
    mutex_unlock(&L.lock);
}
