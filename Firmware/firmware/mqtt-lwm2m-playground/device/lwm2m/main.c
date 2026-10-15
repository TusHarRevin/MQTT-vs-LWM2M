/*
 * lwm2m_device - simulated sensor node speaking LwM2M (Eclipse Wakaama) to
 * an LwM2M server such as Eclipse Leshan.
 *
 *   ./build/lwm2m_device                       # register with 127.0.0.1:5683
 *   ./build/lwm2m_device --interval 1s --count 100
 *   ./build/lwm2m_device --secure              # DTLS-PSK on 5684
 *
 * The device registers, then waits for the server. Notifications only flow
 * once the server OBSERVEs something (Leshan UI or the dashboard). Every
 * interval the sensor values change; each observed resource then produces a
 * NOTIFY.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bench.h"
#include "cli.h"
#include "control.h"
#include "events.h"
#include "lwm2m_client.h"
#include "netsim.h"
#include "objects.h"
#include "pal.h"
#include "sensor.h"
#include "telemetry.h"

static const cli_opt_t lw_opts[] = {
    { "--lifetime", true, "registration lifetime in seconds (default 300)" },
    { "--notify-non", false, "send notifications NON (Wakaama default; latency not measurable)" },
    { "--ack-timeout", true, "CoAP ACK_TIMEOUT for notifications (default 2s)" },
    { NULL, false, NULL },
};

static const char *k_paths[] = { "/3303/0/5700", "/3304/0/5700", "/3315/0/5700", "/3/0/9", "/19/0/0" };

static struct {
    pg_opts_t o;
    lwc_cfg_t cfg;
    uint8_t psk[64];
    netsim_t sim;
    sensor_sim_t sensor;
    bench_t bench;
    bool stop, paused, sample_now, want_register, want_deregister;
    uint32_t samples, notifies, notify_ok, notify_fail;
    bool hinted;
} g;

static void on_notify(const lwc_notify_t *n)
{
    bench_rec_t r;
    memset(&r, 0, sizeof(r));
    r.seq = ++g.notifies;
    r.t_ms = n->t_ms;
    r.lat_ms = n->lat_ms;
    r.ok = n->ok;
    r.retries = (uint8_t)n->retries;
    r.tx_bytes = n->tx_bytes;
    r.rx_bytes = n->rx_bytes;
    r.payload = n->payload;
    if (n->ok) {
        g.notify_ok++;
    } else {
        g.notify_fail++;
    }
    bench_add(&g.bench, &r);
}

static void on_execute(const char *path)
{
    if (strcmp(path, "/3/0/4") == 0) {
        ev_log("LWM2M", "EXECUTE /3/0/4 Reboot - simulated: values reset, session kept");
        sensor_init(&g.sensor, g.o.seed);
    } else if (strcmp(path, "/1/0/8") == 0) {
        ev_log("LWM2M", "EXECUTE /1/0/8 Registration Update Trigger");
        lwc_trigger_update();
    } else {
        ev_log("LWM2M", "EXECUTE %s (reset min/max)", path);
    }
}

static void on_write(const char *path, const char *value)
{
    ev_log("LWM2M", "WRITE %s = \"%s\" applied", path, value);
}

static void apply_command(const char *cmd)
{
    if (strcmp(cmd, "stop") == 0) g.stop = true;
    else if (strcmp(cmd, "pause") == 0) g.paused = true;
    else if (strcmp(cmd, "resume") == 0) g.paused = false;
    else if (strcmp(cmd, "notify") == 0 || strcmp(cmd, "sample") == 0) g.sample_now = true;
    else if (strcmp(cmd, "deregister") == 0) g.want_deregister = true;
    else if (strcmp(cmd, "register") == 0) g.want_register = true;
    else if (strcmp(cmd, "update") == 0) lwc_trigger_update();
    else if (strncmp(cmd, "interval", 8) == 0 && (cmd[8] == '=' || cmd[8] == ' ')) g.o.interval_ms = cli_duration_ms(cmd + 9);
    else {
        ev_log("LWM2M", "unknown command '%s'", cmd);
        return;
    }
    ev_log("LWM2M", "command: %s", cmd);
}

static void send_state(const char *state)
{
    char observed[256];
    const link_t *l = lwc_link();
    lwc_observed(observed, sizeof(observed));
    ev_json("state",
            "\"proto\":\"LWM2M\",\"state\":\"%s\",\"registered\":%s,\"endpoint\":\"%s\","
            "\"observed\":\"%s\",\"interval_ms\":%u,\"samples\":%u,\"notifies\":%u,\"notify_ok\":%u,"
            "\"notify_fail\":%u,\"pending\":%u,\"tx_bytes\":%llu,\"rx_bytes\":%llu,"
            "\"temperature\":%.2f,\"humidity\":%.2f,\"pressure\":%.2f,\"battery\":%u,"
            "\"net\":{\"latency_ms\":%u,\"loss_pct\":%.1f,\"dropped_tx\":%u,\"dropped_rx\":%u},"
            "\"ctl_port\":%u,\"target\":%u,\"notify_con\":%s",
            state, lwc_registered() ? "true" : "false", g.cfg.endpoint, observed, g.o.interval_ms,
            g.samples, g.notifies, g.notify_ok, g.notify_fail, lwc_pending_notifies(),
            (unsigned long long)l->st.tx_bytes, (unsigned long long)l->st.rx_bytes,
            g_lw.now.temperature, g_lw.now.humidity, g_lw.now.pressure, g_lw.now.battery,
            g.sim.cfg.latency_ms, g.sim.cfg.loss * 100.0, g.sim.dropped_tx, g.sim.dropped_rx,
            ctl_port(), g.o.count, g.cfg.notify_con ? "true" : "false");
}

static void sample(void)
{
    SensorData_t d;
    int len;

    sensor_step(&g.sensor, &d);
    objects_update(&d);
    len = telemetry_build(g_lw.blob, sizeof(g_lw.blob), g.cfg.endpoint, g.samples + 1, pal_wall_ms(),
                          &d, g.o.payload);
    g_lw.blob_len = len > 0 ? (size_t)len : 0;
    g.samples++;
    for (size_t i = 0; i < sizeof(k_paths) / sizeof(k_paths[0]); i++) {
        lwc_value_changed(k_paths[i]);
    }
}

static void finish(void)
{
    bench_summary_t s;
    const lwc_session_t *ss = lwc_session();
    char path[600], json[4096];

    if (g.bench.n == 0) {
        return;
    }
    if (ss->have_session) {
        g.bench.connect_ms = ss->connect_ms;
        g.bench.handshake_ms = ss->handshake_ms;
        g.bench.session_ms = ss->register_ms;
        g.bench.hs_bytes = ss->hs_bytes;
        snprintf(g.bench.security, sizeof(g.bench.security), "%s", ss->security);
    }
    bench_summarize(&g.bench, &s);
    if (bench_write_csv(&g.bench, g.o.results_dir, path, sizeof(path)) == 0) {
        ev_log("BENCH", "per-message results written to %s", path);
    }
    bench_summary_json(&g.bench, &s, json, sizeof(json));
    ev_json("run", "\"summary\":%s", json);
    printf("\n==== LwM2M run %s ====\n", g.bench.run_id);
    printf("notifications %u  ok %u  failed %u  success %.1f %%\n", (unsigned)s.n, (unsigned)s.ok,
           (unsigned)s.fail, s.success_pct);
    if (s.min >= 0) {
        printf("latency ms (CON->ACK)  min %.2f  avg %.2f  median %.2f  p95 %.2f  p99 %.2f  max %.2f\n",
               s.min, s.avg, s.median, s.p95, s.p99, s.max);
    } else {
        printf("latency     not measured (NON notifications have no ACK)\n");
    }
    printf("wire bytes/notification  sent %.1f  received %.1f  total %.1f  (payload %.1f)\n",
           s.avg_tx, s.avg_rx, s.avg_total, s.avg_payload);
    printf("connection  %.1f ms to registered", g.bench.session_ms);
    if (g.bench.handshake_ms >= 0) {
        printf(", %s handshake %.1f ms / %llu B", g.bench.security, g.bench.handshake_ms,
               (unsigned long long)g.bench.hs_bytes);
    }
    printf("\n\n");
}

int main(int argc, char **argv)
{
    static char endpoint[64];
    uint64_t next_due, next_state = 0;
    int psk_len;

    setvbuf(stdout, NULL, _IOLBF, 0);
    if (!cli_parse(argc, argv, &g.o, "lwm2m_device", "simulated sensor node over LwM2M", lw_opts)) {
        return 2;
    }
    if (pal_net_init() != 0) {
        return 1;
    }
    snprintf(endpoint, sizeof(endpoint), "%s", g.o.id ? g.o.id : "lwm2m-dev-1");
    if (g.o.port == 0) {
        g.o.port = g.o.secure ? 5684 : 5683;
    }
    g.o.ctl_port = (uint16_t)atoi(cli_get(argc, argv, "--ctl-port", "7102"));
    if (g.o.payload > 1024) {
        fprintf(stderr, "--payload is limited to 1024 bytes for LwM2M (one CoAP block)\n");
        return 2;
    }
    psk_len = link_hex_decode(g.o.psk_hex, g.psk, sizeof(g.psk));
    if (psk_len <= 0) {
        fprintf(stderr, "--psk must be an even number of hex digits\n");
        return 2;
    }

    ev_init(endpoint, "lwm2m", g.o.lab_host, g.o.lab_port);
    ev_set_run(g.o.run_id);
    if (ctl_open(g.o.ctl_port) != 0) {
        ev_log("SYS", "control port %u busy - dashboard commands disabled for this device",
               (unsigned)g.o.ctl_port);
    }
    netsim_init(&g.sim, &g.o.net);
    sensor_init(&g.sensor, g.o.seed);
    bench_init(&g.bench, "LWM2M", g.o.run_id);

    g.cfg.endpoint = endpoint;
    g.cfg.host = g.o.host;
    g.cfg.port = g.o.port;
    g.cfg.secure = g.o.secure;
    g.cfg.psk_identity = g.o.psk_id ? g.o.psk_id : endpoint;
    g.cfg.psk = g.psk;
    g.cfg.psk_len = (size_t)psk_len;
    g.cfg.lifetime_s = (uint32_t)atoi(cli_get(argc, argv, "--lifetime", "300"));
    g.cfg.notify_con = cli_get(argc, argv, "--notify-non", NULL) == NULL;
    g.cfg.ack_timeout_ms = cli_duration_ms(cli_get(argc, argv, "--ack-timeout", "2s"));
    g.cfg.dtls_timeout_ms = g.o.timeout_ms;
    g.cfg.sim = &g.sim;
    g.cfg.on_notify = on_notify;
    snprintf(g.bench.config, sizeof(g.bench.config),
             "\"interval_ms\":%u,\"count\":%u,\"payload_target\":%u,\"secure\":%s,\"latency_ms\":%u,"
             "\"loss_pct\":%.1f,\"profile\":\"%s\",\"notify\":\"%s\",\"seed\":%llu",
             g.o.interval_ms, g.o.count, g.o.payload, g.o.secure ? "true" : "false",
             g.o.net.latency_ms, g.o.net.loss * 100.0, g.o.profile, g.cfg.notify_con ? "CON" : "NON",
             (unsigned long long)g.o.seed);
    g_lw.on_execute = on_execute;
    g_lw.on_write = on_write;
    snprintf(g_lw.serial, sizeof(g_lw.serial), "PG-%08llX", (unsigned long long)(g.o.seed * 2654435761ULL & 0xffffffffULL));
    snprintf(g_lw.fw_version, sizeof(g_lw.fw_version), "1.0.0");
    snprintf(g_lw.utc_offset, sizeof(g_lw.utc_offset), "+00:00");
    snprintf(g_lw.timezone, sizeof(g_lw.timezone), "UTC");

    {
        SensorData_t d;
        sensor_peek(&g.sensor, &d);
        objects_update(&d);
        g_lw.blob_len = (size_t)telemetry_build(g_lw.blob, sizeof(g_lw.blob), endpoint, 0,
                                                pal_wall_ms(), &d, g.o.payload);
    }

    ev_log("SYS", "LWM2M DEVICE START endpoint=%s server=%s://%s:%u interval=%ums count=%u%s",
           endpoint, g.o.secure ? "coaps" : "coap", g.o.host, (unsigned)g.o.port, g.o.interval_ms,
           g.o.count, g.o.secure ? " DTLS-PSK" : "");
    if (netsim_active(&g.sim)) {
        ev_log("NET", "simulated network: %u ms one-way latency, %.1f %% loss (app-level)",
               g.sim.cfg.latency_ms, g.sim.cfg.loss * 100.0);
    }
    if (lwc_start(&g.cfg) != 0) {
        ev_log("SYS", "could not start the LwM2M client");
        return 1;
    }

    next_due = pal_now_us() + (uint64_t)g.o.interval_ms * 1000ULL;
    while (!g.stop) {
        char cmd[128];
        uint64_t now;

        while (ctl_poll(cmd, sizeof(cmd)) > 0) {
            apply_command(cmd);
        }
        if (g.want_deregister) {
            g.want_deregister = false;
            if (lwc_running()) {
                lwc_deregister();
            }
        }
        if (g.want_register) {
            g.want_register = false;
            if (!lwc_running()) {
                lwc_start(&g.cfg);
            } else {
                ev_log("LWM2M", "already running - use 'deregister' first");
            }
        }

        lwc_step(50);
        now = pal_now_us();

        if (lwc_registered() && !g.hinted) {
            char obs[256];
            lwc_observed(obs, sizeof(obs));
            if (obs[0] == '\0') {
                ev_log("LWM2M", "waiting for the server to OBSERVE a resource (dashboard or Leshan UI)");
            }
            g.hinted = true;
        }
        /* With --count N: stop producing once N are sent or in flight, then let the
         * outstanding ones finish (ACK or retransmissions exhausted) so in-flight
         * messages are never counted as failures. */
        bool budget = g.o.count == 0 || g.notifies + lwc_pending_notifies() < g.o.count;
        if (((now >= next_due && !g.paused) || g.sample_now) && lwc_running() && budget) {
            g.sample_now = false;
            sample();
            next_due += (uint64_t)g.o.interval_ms * 1000ULL;
            if (next_due < now) {
                next_due = now + (uint64_t)g.o.interval_ms * 1000ULL;
            }
        }
        if (now >= next_state) {
            send_state(lwc_registered() ? "registered" : (lwc_running() ? "registering" : "deregistered"));
            next_state = now + 1000000ULL;
        }
        if (g.o.count && g.notifies >= g.o.count) {
            ev_log("BENCH", "%u notifications resolved - run complete", g.notifies);
            break;
        }
    }

    if (lwc_running()) {
        lwc_deregister();
    }
    send_state("stopped");
    finish();
    bench_free(&g.bench);
    objects_free();
    ctl_close();
    return 0;
}
