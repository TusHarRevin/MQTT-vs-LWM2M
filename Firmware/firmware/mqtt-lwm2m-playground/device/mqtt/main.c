/*
 * mqtt_device - simulated sensor node publishing telemetry over MQTT.
 *
 *   ./build/mqtt_device                         # QoS 1, every 10 s, forever
 *   ./build/mqtt_device --qos 2 --interval 1s --count 100
 *   ./build/mqtt_device --secure --ca servers/mosquitto/certs/ca.crt
 *
 * Flow: TCP connect -> [TLS handshake] -> CONNECT/CONNACK -> SUBSCRIBE cmd
 * topic -> PUBLISH telemetry every interval -> DISCONNECT.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bench.h"
#include "cli.h"
#include "control.h"
#include "events.h"
#include "link.h"
#include "mqtt_client.h"
#include "netsim.h"
#include "pal.h"
#include "sensor.h"
#include "telemetry.h"

static const cli_opt_t mqtt_opts[] = {
    { "--qos", true, "0, 1 or 2 (default 1)" },
    { "--no-probe", false, "QoS 0: don't send the PINGREQ latency probe" },
    { "--keepalive", true, "MQTT keep-alive seconds (default 60)" },
    { "--topic", true, "topic prefix (default playground)" },
    { "--user", true, "MQTT username" },
    { "--pass", true, "MQTT password" },
    { NULL, false, NULL },
};

static struct {
    pg_opts_t o;
    int qos;
    bool probe;
    bool stop, paused, publish_now;
    char topic_tel[128], topic_cmd[128];
    netsim_t sim;
    link_t link;
    mqtt_t mqtt;
    bench_t bench;
    sensor_sim_t sensor;
    uint32_t published, acked, failed, received;
    double connect_lat_ms;
    uint16_t keepalive_s;
    const char *user, *pass;
} g;

static void apply_command(const char *cmd)
{
    if (strcmp(cmd, "stop") == 0) {
        g.stop = true;
    } else if (strcmp(cmd, "pause") == 0) {
        g.paused = true;
    } else if (strcmp(cmd, "resume") == 0) {
        g.paused = false;
    } else if (strcmp(cmd, "publish") == 0) {
        g.publish_now = true;
    } else if (strncmp(cmd, "interval", 8) == 0 && (cmd[8] == '=' || cmd[8] == ' ')) {
        g.o.interval_ms = cli_duration_ms(cmd + 9);
    } else if (strncmp(cmd, "qos", 3) == 0 && (cmd[3] == '=' || cmd[3] == ' ')) {
        int q = atoi(cmd + 4);
        if (q >= 0 && q <= 2) {
            g.qos = q;
        }
    } else {
        ev_log("MQTT", "unknown command '%s'", cmd);
        return;
    }
    ev_log("MQTT", "command: %s", cmd);
}

static void on_message(const char *topic, size_t tlen, const uint8_t *payload, size_t len, void *u)
{
    char cmd[128];
    (void)topic; (void)tlen; (void)u;
    g.received++;
    snprintf(cmd, sizeof(cmd), "%.*s", (int)(len < 127 ? len : 127), (const char *)payload);
    apply_command(cmd);
}

static void send_state(const char *state, const SensorData_t *d)
{
    ev_json("state",
            "\"proto\":\"MQTT\",\"state\":\"%s\",\"qos\":%d,\"interval_ms\":%u,\"published\":%u,"
            "\"acked\":%u,\"failed\":%u,\"received\":%u,\"tx_bytes\":%llu,\"rx_bytes\":%llu,"
            "\"temperature\":%.2f,\"humidity\":%.2f,\"pressure\":%.2f,\"battery\":%u,"
            "\"net\":{\"latency_ms\":%u,\"loss_pct\":%.1f,\"dropped_tx\":%u,\"dropped_rx\":%u,"
            "\"tcp_retx\":%u},\"ctl_port\":%u,\"target\":%u",
            state, g.qos, g.o.interval_ms, g.published, g.acked, g.failed, g.received,
            (unsigned long long)g.link.st.tx_bytes, (unsigned long long)g.link.st.rx_bytes,
            d->temperature, d->humidity, d->pressure, d->battery, g.sim.cfg.latency_ms,
            g.sim.cfg.loss * 100.0, g.sim.dropped_tx, g.sim.dropped_rx, g.sim.tcp_retx,
            ctl_port(), g.o.count);
}

/* TCP connect, TLS handshake, MQTT CONNECT and SUBSCRIBE. */
static int session_up(bool first)
{
    link_security_t sec = { 0 };
    mqtt_cfg_t cfg = { 0 };
    uint64_t t0 = pal_now_us();
    char detail[160];
    double sub_lat;
    int rc;

    sec.enabled = g.o.secure;
    sec.ca_file = g.o.ca_file;
    sec.server_name = g.o.tls_name;
    sec.tls12_only = g.o.tls12;

    rc = link_open(&g.link, LINK_TCP, g.o.host, g.o.port, &sec, &g.sim, g.o.timeout_ms);
    if (rc != 0) {
        ev_log("NET", "%s", g.link.err);
        return -1;
    }
    {
        ev_t e = EV_INIT;
        e.proto = "NET";
        e.to = "BROKER";
        e.type = "TCP CONNECT";
        e.lat_ms = g.link.st.connect_ms;
        snprintf(detail, sizeof(detail), "%s:%u", g.o.host, (unsigned)g.o.port);
        e.detail = detail;
        ev_emit(&e);
    }
    if (g.o.secure) {
        ev_t e = EV_INIT;
        char extra[192];
        e.proto = "TLS";
        e.to = "BROKER";
        e.type = "HANDSHAKE";
        e.lat_ms = g.link.st.handshake_ms;
        e.len = (int)(g.link.st.hs_tx_bytes + g.link.st.hs_rx_bytes);
        snprintf(detail, sizeof(detail), "%s %s, sent %llu B, received %llu B%s", g.link.st.security,
                 g.link.st.cipher, (unsigned long long)g.link.st.hs_tx_bytes,
                 (unsigned long long)g.link.st.hs_rx_bytes,
                 g.o.ca_file ? ", server verified" : ", SERVER NOT VERIFIED (no --ca)");
        e.detail = detail;
        snprintf(extra, sizeof(extra), "\"hs_tx\":%llu,\"hs_rx\":%llu,\"cipher\":\"%s\"",
                 (unsigned long long)g.link.st.hs_tx_bytes, (unsigned long long)g.link.st.hs_rx_bytes,
                 g.link.st.cipher);
        e.extra = extra;
        ev_set_security(g.link.st.security);
        ev_emit(&e);
    }

    cfg.client_id = g.o.id;
    cfg.keepalive_s = g.keepalive_s;
    cfg.timeout_ms = g.o.timeout_ms;
    cfg.username = g.user;
    cfg.password = g.pass;
    cfg.on_message = on_message;
    rc = mqtt_connect(&g.mqtt, &g.link, &cfg, &g.connect_lat_ms);
    if (rc != MQTT_OK) {
        link_close(&g.link);
        return -1;
    }
    if (mqtt_subscribe(&g.mqtt, g.topic_cmd, 1, &sub_lat) != MQTT_OK) {
        ev_log("MQTT", "subscribe to %s failed (continuing without commands)", g.topic_cmd);
    }
    if (first) {
        g.bench.connect_ms = g.link.st.connect_ms;
        g.bench.handshake_ms = g.o.secure ? g.link.st.handshake_ms : -1.0;
        g.bench.hs_bytes = g.link.st.hs_tx_bytes + g.link.st.hs_rx_bytes;
        /* time to "ready": socket + handshake + CONNECT/CONNACK */
        g.bench.session_ms = pal_ms_since(t0) - sub_lat;
        snprintf(g.bench.security, sizeof(g.bench.security), "%s", g.link.st.security);
    }
    ev_log("MQTT", "session ready in %.1f ms (tcp %.1f, %s %.1f, connect %.1f)",
           pal_ms_since(t0) - sub_lat, g.link.st.connect_ms, g.o.secure ? g.link.st.security : "tls",
           g.o.secure ? g.link.st.handshake_ms : 0.0, g.connect_lat_ms);
    return 0;
}

static void publish_one(uint32_t seq)
{
    SensorData_t d;
    char payload[MQTT_BUF_SIZE - 512], detail[160];
    mqtt_pub_result_t res;
    bench_rec_t rec = { 0 };
    int len, rc;

    sensor_step(&g.sensor, &d);
    len = telemetry_build(payload, sizeof(payload), g.o.id, seq, pal_wall_ms(), &d, g.o.payload);
    if (len < 0) {
        ev_log("MQTT", "payload of %u bytes is too large (max %u)", g.o.payload,
               (unsigned)sizeof(payload) - 1);
        g.stop = true;
        return;
    }
    snprintf(detail, sizeof(detail), "temperature=%.2f humidity=%.2f pressure=%.2f battery=%u",
             d.temperature, d.humidity, d.pressure, d.battery);

    rec.seq = seq;
    rec.t_ms = pal_wall_ms();
    rec.payload = (uint32_t)len;
    rc = mqtt_publish(&g.mqtt, g.topic_tel, (const uint8_t *)payload, (size_t)len, g.qos, g.probe,
                      detail, &res);
    g.published++;
    rec.tx_bytes = res.tx_bytes;
    rec.rx_bytes = res.rx_bytes;
    if (rc == MQTT_OK && (g.qos > 0 || g.probe)) {
        rec.ok = 1;
        rec.lat_ms = res.lat_ms;
        g.acked++;
    } else if (rc == MQTT_OK) {
        rec.ok = 1; /* QoS 0 without probe: sent, latency not measurable */
        rec.lat_ms = -1;
    } else {
        rec.ok = 0;
        rec.lat_ms = -1;
        g.failed++;
        ev_log("MQTT", "message %u failed (%s)", seq,
               rc == MQTT_TIMEOUT ? "no acknowledgement within timeout" : "connection lost");
        if (rc == MQTT_TIMEOUT) {
            g.mqtt.connected = false; /* the stream state is unknown now */
        }
    }
    bench_add(&g.bench, &rec);
    send_state("connected", &d);
}

static void finish(void)
{
    bench_summary_t s;
    char path[600], json[4096];

    if (g.bench.n == 0) {
        return;
    }
    bench_summarize(&g.bench, &s);
    if (bench_write_csv(&g.bench, g.o.results_dir, path, sizeof(path)) == 0) {
        ev_log("BENCH", "per-message results written to %s", path);
    }
    bench_summary_json(&g.bench, &s, json, sizeof(json));
    ev_json("run", "\"summary\":%s", json);
    printf("\n==== MQTT run %s ====\n", g.bench.run_id);
    printf("messages %u  ok %u  failed %u  success %.1f %%\n", (unsigned)s.n, (unsigned)s.ok,
           (unsigned)s.fail, s.success_pct);
    if (s.min >= 0) {
        printf("latency ms  min %.2f  avg %.2f  median %.2f  p95 %.2f  p99 %.2f  max %.2f\n", s.min,
               s.avg, s.median, s.p95, s.p99, s.max);
    } else {
        printf("latency     not measured (QoS 0 without probe has no acknowledgement)\n");
    }
    printf("wire bytes/message  sent %.1f  received %.1f  total %.1f  (payload %.1f)\n", s.avg_tx,
           s.avg_rx, s.avg_total, s.avg_payload);
    printf("connection  %.1f ms to ready", g.bench.session_ms);
    if (g.bench.handshake_ms >= 0) {
        printf(", %s handshake %.1f ms / %llu B", g.bench.security, g.bench.handshake_ms,
               (unsigned long long)g.bench.hs_bytes);
    }
    printf("\n\n");
}

int main(int argc, char **argv)
{
    const char *v;
    uint32_t seq = 0;
    uint64_t next_due;
    SensorData_t d;

    setvbuf(stdout, NULL, _IOLBF, 0);
    if (!cli_parse(argc, argv, &g.o, "mqtt_device", "simulated sensor node over MQTT", mqtt_opts)) {
        return 2;
    }
    if (pal_net_init() != 0) {
        fprintf(stderr, "network init failed\n");
        return 1;
    }
    if (g.o.id == NULL) {
        g.o.id = "mqtt-dev-1";
    }
    if (g.o.port == 0) {
        g.o.port = g.o.secure ? 8883 : 1883;
    }
    g.qos = atoi(cli_get(argc, argv, "--qos", "1"));
    if (g.qos < 0 || g.qos > 2) {
        fprintf(stderr, "--qos must be 0, 1 or 2\n");
        return 2;
    }
    g.probe = cli_get(argc, argv, "--no-probe", NULL) == NULL;
    g.keepalive_s = (uint16_t)atoi(cli_get(argc, argv, "--keepalive", "60"));
    g.user = cli_get(argc, argv, "--user", NULL);
    g.pass = cli_get(argc, argv, "--pass", NULL);
    v = cli_get(argc, argv, "--topic", "playground");
    snprintf(g.topic_tel, sizeof(g.topic_tel), "%s/%s/telemetry", v, g.o.id);
    snprintf(g.topic_cmd, sizeof(g.topic_cmd), "%s/%s/cmd", v, g.o.id);
    g.o.ctl_port = (uint16_t)atoi(cli_get(argc, argv, "--ctl-port", "7101"));

    ev_init(g.o.id, "mqtt", g.o.lab_host, g.o.lab_port);
    ev_set_run(g.o.run_id);
    if (ctl_open(g.o.ctl_port) != 0) {
        ev_log("SYS", "control port %u busy - dashboard commands disabled for this device",
               (unsigned)g.o.ctl_port);
    }
    netsim_init(&g.sim, &g.o.net);
    sensor_init(&g.sensor, g.o.seed);
    bench_init(&g.bench, "MQTT", g.o.run_id);
    snprintf(g.bench.config, sizeof(g.bench.config),
             "\"qos\":%d,\"interval_ms\":%u,\"count\":%u,\"payload_target\":%u,\"secure\":%s,"
             "\"latency_ms\":%u,\"loss_pct\":%.1f,\"profile\":\"%s\",\"probe\":%s,\"seed\":%llu",
             g.qos, g.o.interval_ms, g.o.count, g.o.payload, g.o.secure ? "true" : "false",
             g.o.net.latency_ms, g.o.net.loss * 100.0, g.o.profile, g.probe ? "true" : "false",
             (unsigned long long)g.o.seed);

    ev_log("SYS", "MQTT DEVICE START id=%s broker=%s:%u qos=%d interval=%ums count=%u%s",
           g.o.id, g.o.host, (unsigned)g.o.port, g.qos, g.o.interval_ms, g.o.count,
           g.o.secure ? " TLS" : "");
    if (netsim_active(&g.sim)) {
        ev_log("NET", "simulated network: %u ms one-way latency, %.1f %% loss (app-level)",
               g.sim.cfg.latency_ms, g.sim.cfg.loss * 100.0);
    }

    if (session_up(true) != 0) {
        sensor_peek(&g.sensor, &d);
        send_state("error", &d);
        ev_log("SYS", "could not start a session - see docs/troubleshooting.md");
        return 1;
    }
    sensor_peek(&g.sensor, &d);
    send_state("connected", &d);

    next_due = pal_now_us();
    while (!g.stop) {
        char cmd[128];
        uint64_t now = pal_now_us();

        while (ctl_poll(cmd, sizeof(cmd)) > 0) {
            apply_command(cmd);
        }
        if (g.stop) {
            break;
        }
        if (!g.mqtt.connected) {
            link_close(&g.link);
            ev_log("MQTT", "reconnecting...");
            if (session_up(false) != 0) {
                pal_sleep_ms(1000);
                continue;
            }
        }
        if ((now >= next_due && !g.paused) || g.publish_now) {
            g.publish_now = false;
            publish_one(++seq);
            next_due += (uint64_t)g.o.interval_ms * 1000ULL;
            if (next_due < pal_now_us()) {
                next_due = pal_now_us(); /* fell behind (slow network): don't burst */
            }
            if (g.o.count && seq >= g.o.count) {
                break;
            }
            continue;
        }
        {
            uint64_t wait_us = next_due > now ? next_due - now : 0;
            uint32_t wait_ms = (uint32_t)(wait_us / 1000);
            if (wait_ms > 100) {
                wait_ms = 100;
            }
            if (mqtt_poll(&g.mqtt, wait_ms) != MQTT_OK) {
                g.mqtt.connected = false;
            }
        }
    }

    mqtt_disconnect(&g.mqtt);
    link_close(&g.link);
    sensor_peek(&g.sensor, &d);
    send_state("stopped", &d);
    finish();
    bench_free(&g.bench);
    ctl_close();
    return 0;
}
