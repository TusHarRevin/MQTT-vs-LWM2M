/*
 * lab - the playground's local web server, written in C like everything else.
 *
 *   ./build/lab                     then open http://127.0.0.1:8000
 *
 * It does five things:
 *   1. serves the dashboard (dashboard/www)
 *   2. receives protocol events from the devices (UDP 127.0.0.1:7000) and
 *      streams them to the browser (Server-Sent Events, /api/events)
 *   3. starts/stops the simulated devices with the settings chosen in the UI
 *   4. watches the servers: its own MQTT subscriber on the broker, and
 *      Leshan's REST API + event stream (proxied at /leshan/...)
 *   5. serves firmware images for the SIMULATED OTA exercise (/ota/...)
 *   6. optionally drives a real board (STM32L452 + EC200U, --hw-serial):
 *      runs are started over its USB serial port and its events arrive the
 *      same way, one JSON object per line
 *
 * It binds to 127.0.0.1 only. It is a lab tool, not something to expose.
 */

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../../ota/fw_image.h"
#include "events.h"
#include "http.h"
#include "lab.h"
#include "mbedtls/sha256.h"
#include "pal.h"
#include "proc.h"
#include "serial.h"

lab_t L;

/* ---- SSE hub + replay buffer ----------------------------------------------- */

#define RING 3000
#define MAX_SSE 16

static struct {
    lab_mutex_t lock;
    char *ring[RING];
    size_t head, count;
    pal_sock_t clients[MAX_SSE];
    int nclients;
} hub;

void lab_broadcast(const char *json)
{
    size_t len = strlen(json);
    char *frame = malloc(len + 16);
    if (frame == NULL) {
        return;
    }
    memcpy(frame, "data: ", 6);
    memcpy(frame + 6, json, len);
    memcpy(frame + 6 + len, "\n\n", 3);

    mutex_lock(&hub.lock);
    free(hub.ring[hub.head]);
    hub.ring[hub.head] = frame;
    hub.head = (hub.head + 1) % RING;
    if (hub.count < RING) {
        hub.count++;
    }
    for (int i = 0; i < hub.nclients;) {
        if (pal_sock_send_all(hub.clients[i], frame, len + 8) != 0) {
            pal_sock_close(hub.clients[i]);
            hub.clients[i] = hub.clients[--hub.nclients];
        } else {
            i++;
        }
    }
    mutex_unlock(&hub.lock);
}

static void hub_clear(void)
{
    mutex_lock(&hub.lock);
    for (size_t i = 0; i < RING; i++) {
        free(hub.ring[i]);
        hub.ring[i] = NULL;
    }
    hub.head = hub.count = 0;
    mutex_unlock(&hub.lock);
}

static void sse_attach(pal_sock_t s)
{
    static const char head[] = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                               "Cache-Control: no-store\r\nConnection: keep-alive\r\n\r\n"
                               "retry: 2000\n\n";
    mutex_lock(&hub.lock);
    if (pal_sock_send_all(s, head, sizeof(head) - 1) == 0) {
        size_t start = (hub.head + RING - hub.count) % RING;
        for (size_t i = 0; i < hub.count; i++) {
            const char *f = hub.ring[(start + i) % RING];
            if (f && pal_sock_send_all(s, f, strlen(f)) != 0) {
                break;
            }
        }
        if (hub.nclients < MAX_SSE) {
            hub.clients[hub.nclients++] = s;
            s = PAL_SOCK_INVALID;
        }
    }
    mutex_unlock(&hub.lock);
    if (s != PAL_SOCK_INVALID) {
        pal_sock_close(s);
    }
}

static void heartbeat(void *arg)
{
    (void)arg;
    for (;;) {
        pal_sleep_ms(15000);
        mutex_lock(&hub.lock);
        for (int i = 0; i < hub.nclients;) {
            if (pal_sock_send_all(hub.clients[i], ": ping\n\n", 8) != 0) {
                pal_sock_close(hub.clients[i]);
                hub.clients[i] = hub.clients[--hub.nclients];
            } else {
                i++;
            }
        }
        mutex_unlock(&hub.lock);
    }
}

static void lab_event(const char *kind, const char *event, const char *fmt, ...)
{
    char msg[512], esc[1100], out[1400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    printf("[lab] %s\n", msg);
    fflush(stdout);
    json_escape(esc, sizeof(esc), msg, sizeof(msg));
    snprintf(out, sizeof(out), "{\"k\":\"lab\",\"t\":%llu,\"kind\":\"%s\",\"event\":\"%s\",\"msg\":\"%s\"}",
             (unsigned long long)pal_wall_ms(), kind, event, esc);
    lab_broadcast(out);
}

/* ---- devices ----------------------------------------------------------------- */

enum { DEV_MQTT, DEV_LWM2M, DEV_OTA, DEV_COUNT };
static const char *dev_names[DEV_COUNT] = { "mqtt", "lwm2m", "ota" };
static const char *dev_exe[DEV_COUNT] = { "mqtt_device", "lwm2m_device", "ota_device" };
static const uint16_t dev_ctl[DEV_COUNT] = { 7101, 7102, 7103 };

static struct {
    proc_t proc;
    char run[64];
    char *state; /* last {"k":"state"} JSON */
    uint64_t started;
    bool hw;     /* this (or the last) run is on the board, not a PC process */
    int hw_exit;
} dev[DEV_COUNT];

static int dev_index(const char *name)
{
    for (int i = 0; i < DEV_COUNT; i++) {
        if (name && strcmp(name, dev_names[i]) == 0) {
            return i;
        }
    }
    return -1;
}

/* ---- the board (STM32L452 + EC200U over its ST-LINK serial port) ---------- */

static struct {
    lab_mutex_t lock;      /* guards ser (writes vs. the reader reconnecting) */
    serial_t *ser;
    bool online;           /* port open */
    char state[24];        /* boot, modem, ready, modem_error, running, exit, fault, offline */
    char msg[256];         /* last modem progress / error text */
    char *status;          /* last full {"k":"hw",...} status object */
    int busy;              /* device index running on the board, -1 = idle */
} hw = { .busy = -1, .state = "offline" };

/* Caller holds L.lock. */
static bool dev_running(int d)
{
    return dev[d].hw ? hw.busy == d : dev[d].proc.running;
}

static int hw_send(const char *line)
{
    int rc = -1;
    mutex_lock(&hw.lock);
    if (hw.ser != NULL && serial_write(hw.ser, line, strlen(line)) == 0 && serial_write(hw.ser, "\n", 1) == 0) {
        rc = 0;
    }
    mutex_unlock(&hw.lock);
    return rc;
}

/* The board has no clock of its own: give it ours (and our UTC offset, for
 * its console timestamps). */
static void hw_send_time(void)
{
    uint64_t ms = pal_wall_ms();
    time_t now = (time_t)(ms / 1000);
    struct tm lt, gt;
    char line[64];
#ifdef _WIN32
    localtime_s(&lt, &now);
    gmtime_s(&gt, &now);
#else
    localtime_r(&now, &lt);
    gmtime_r(&now, &gt);
#endif
    gt.tm_isdst = lt.tm_isdst;
    snprintf(line, sizeof(line), "time %llu %ld", (unsigned long long)ms,
             (long)(difftime(mktime(&lt), mktime(&gt)) / 60));
    hw_send(line);
}

/* Wait (up to 90 s - the board reboots after every run) until it is idle with
 * the modem connected. Returns 0 or an HTTP status with err filled in. */
static int hw_wait_ready(char *err, size_t errcap)
{
    if (!L.hw_port[0]) {
        snprintf(err, errcap, "no board configured - start the lab with --hw-serial COMx (see hardware/stm32l452-ec200u/README.md)");
        return 400;
    }
    for (int i = 0; i < 450; i++) {
        bool online, ready, failed;
        mutex_lock(&L.lock);
        online = hw.online;
        ready = online && hw.busy < 0 && strcmp(hw.state, "ready") == 0;
        failed = online && strcmp(hw.state, "modem_error") == 0;
        if (failed) {
            snprintf(err, errcap, "board: modem not connected - %s", hw.msg);
        }
        mutex_unlock(&L.lock);
        if (ready) {
            return 0;
        }
        if (failed) {
            return 503;
        }
        if (!online && i >= 10) {
            snprintf(err, errcap, "board not connected on %s (is it plugged in? another program using the port?)", L.hw_port);
            return 503;
        }
        pal_sleep_ms(200);
    }
    mutex_lock(&L.lock);
    snprintf(err, errcap, "board not ready after 90 s (state: %s %s)", hw.state, hw.msg);
    mutex_unlock(&L.lock);
    return 504;
}

static void json_get_str(const char *json, const char *key, char *out, size_t cap)
{
    char pat[48];
    const char *p;
    out[0] = '\0';
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    if ((p = strstr(json, pat)) != NULL) {
        size_t i = 0;
        for (p += strlen(pat); *p && *p != '"' && i + 1 < cap; p++) {
            out[i++] = *p == '\\' && p[1] ? *++p : *p;
        }
        out[i] = '\0';
    }
}

static void lab_event(const char *kind, const char *event, const char *fmt, ...);

/* A {"k":"hw",...} line: the board's own state. */
static void hw_status_line(const char *line)
{
    char st[24], kind[16], msg[256];
    const char *c = strstr(line, "\"code\":");
    int d, code = c ? atoi(c + 7) : 0;
    bool booted;

    json_get_str(line, "state", st, sizeof(st));
    json_get_str(line, "kind", kind, sizeof(kind));
    json_get_str(line, "msg", msg, sizeof(msg));
    d = dev_index(kind);

    mutex_lock(&L.lock);
    snprintf(hw.state, sizeof(hw.state), "%s", st);
    booted = strcmp(st, "boot") == 0;
    if (strcmp(st, "running") == 0 && d >= 0) {
        hw.busy = d;
    } else if (strcmp(st, "exit") == 0 || booted || strcmp(st, "fault") == 0) {
        if (strcmp(st, "exit") == 0 && d >= 0) {
            dev[d].hw_exit = code;
        } else if (hw.busy >= 0) {
            dev[hw.busy].hw_exit = -1; /* the board restarted mid-run */
        }
        hw.busy = -1;
    }
    if (strcmp(st, "ready") == 0 || strcmp(st, "modem_error") == 0) {
        char err[200];
        free(hw.status);
        hw.status = strdup(line);
        json_get_str(line, "err", err, sizeof(err));
        snprintf(hw.msg, sizeof(hw.msg), "%s", err);
    } else if (msg[0]) {
        snprintf(hw.msg, sizeof(hw.msg), "%s", msg);
    }
    mutex_unlock(&L.lock);

    if (booted) {
        hw_send_time();
        lab_event("hw", "boot", "board booted - connecting the modem");
    } else if (strcmp(st, "ready") == 0) {
        char op[48], ip[48];
        const char *r = strstr(line, "\"rssi\":");
        json_get_str(line, "operator", op, sizeof(op));
        json_get_str(line, "ip", ip, sizeof(ip));
        lab_event("hw", "ready", "board ready: %s, signal %d (CSQ), IP %s", op, r ? atoi(r + 7) : 99, ip);
    } else if (strcmp(st, "modem_error") == 0 || strcmp(st, "fault") == 0) {
        lab_event("hw", st, "board: %s", hw.msg);
    } else if (strcmp(st, "exit") == 0 && d >= 0) {
        lab_event(dev_names[d], "exit", "%s on the board finished (code %d)", dev_exe[d], code);
    }
}

static void reaper(void *arg)
{
    (void)arg;
    for (;;) {
        pal_sleep_ms(300);
        for (int i = 0; i < DEV_COUNT; i++) {
            bool was;
            mutex_lock(&L.lock);
            was = dev[i].proc.running;
            if (was && proc_poll(&dev[i].proc)) {
                mutex_unlock(&L.lock);
                lab_event(dev_names[i], "exit", "%s exited (code %d)", dev_exe[i], dev[i].proc.exit_code);
                continue;
            }
            mutex_unlock(&L.lock);
        }
    }
}

static void send_ctl(int d, const char *cmd)
{
    pal_sock_t s;
    if (dev[d].hw) {
        hw_send(cmd);
        return;
    }
    if (pal_udp_bind(&s, "127.0.0.1", 0) == 0) {
        pal_udp_sendto(s, "127.0.0.1", dev_ctl[d], cmd, strlen(cmd));
        pal_sock_close(s);
    }
}

static bool safe_token(const char *v)
{
    size_t n = strlen(v);
    if (n == 0 || n > 40) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (!isalnum((unsigned char)v[i]) && v[i] != '.' && v[i] != '_' && v[i] != '-') {
            return false;
        }
    }
    return true;
}

/* PUT the device's PSK into Leshan's security store before a DTLS run. */
static int leshan_put_psk(const char *endpoint, const char *identity, const char *key_hex)
{
    upstream_t *s;
    char body[512], req[1024], resp[512];
    int n, status = 0;

    n = snprintf(body, sizeof(body),
                 "{\"endpoint\":\"%s\",\"tls\":{\"mode\":\"psk\",\"details\":{\"identity\":\"%s\",\"key\":\"%s\"}}}",
                 endpoint, identity, key_hex);
    snprintf(req, sizeof(req),
             "PUT /api/security/clients/ HTTP/1.0\r\nHost: %s\r\nContent-Type: application/json\r\n"
             "Content-Length: %d\r\n\r\n%s",
             L.leshan_host, n, body);
    if ((s = leshan_connect()) == NULL) {
        return -1;
    }
    upstream_send_all(s, req, strlen(req));
    n = upstream_recv(s, resp, sizeof(resp) - 1, 10000);
    upstream_close(s);
    if (n > 0) {
        resp[n] = '\0';
        sscanf(resp, "HTTP/%*s %d", &status);
    }
    return status == 200 ? 0 : -1;
}

/* Security OFF: remove any PSK left from a DTLS run, otherwise Leshan
 * answers a plain-CoAP REGISTER with 4.03 Forbidden. */
static void leshan_delete_psk(const char *endpoint)
{
    upstream_t *s;
    char req[512], resp[256];
    snprintf(req, sizeof(req), "DELETE /api/security/clients/%s HTTP/1.0\r\nHost: %s\r\n\r\n", endpoint,
             L.leshan_host);
    if ((s = leshan_connect()) == NULL) {
        return;
    }
    upstream_send_all(s, req, strlen(req));
    (void)upstream_recv(s, resp, sizeof(resp) - 1, 5000);
    upstream_close(s);
}

static int dev_start(int d, const http_req_t *r, char *err, size_t errcap)
{
    static const char *allowed[DEV_COUNT][16] = {
        { "qos", "interval", "count", "payload", "latency", "loss", "profile", "seed", "id",
          "keepalive", "timeout", "rto", NULL },
        { "interval", "count", "payload", "latency", "loss", "profile", "seed", "id", "lifetime",
          "ack-timeout", "timeout", "rto", NULL },
        { "image", "chunk", "action", "latency", "loss", "profile", "seed", "timeout", "rto", NULL },
    };
    static char store[48][64];
    char *argv[64];
    char exe[600], v[64], portbuf[8], labaddr[32], ctlbuf[8];
    int a = 0, st = 0;
    bool secure, on_hw;

    on_hw = http_param(r, "target", v, sizeof(v)) && strcmp(v, "hw") == 0;
    mutex_lock(&L.lock);
    if (dev_running(d) || (on_hw && hw.busy >= 0)) {
        int busy = on_hw && hw.busy >= 0 ? hw.busy : d;
        mutex_unlock(&L.lock);
        snprintf(err, errcap, "%s is already running%s - stop it first", dev_exe[busy], dev[busy].hw ? " on the board" : "");
        return 409;
    }
    mutex_unlock(&L.lock);

    if (on_hw) {
        if (d == DEV_OTA) {
            snprintf(err, errcap, "the simulated OTA runs on the PC only - switch the device to 'PC simulation'");
            return 400;
        }
        if ((st = hw_wait_ready(err, errcap)) != 0) {
            return st;
        }
        snprintf(exe, sizeof(exe), "%s", dev_exe[d]);
    } else {
#ifdef _WIN32
        snprintf(exe, sizeof(exe), "%s/%s.exe", L.bin_dir, dev_exe[d]);
#else
        snprintf(exe, sizeof(exe), "%s/%s", L.bin_dir, dev_exe[d]);
#endif
        if (!pal_file_exists(exe)) {
            snprintf(err, errcap, "%s not found - did you build the project?", exe);
            return 500;
        }
    }
    secure = http_param(r, "secure", v, sizeof(v)) && strcmp(v, "1") == 0;

    /* Device id / LwM2M endpoint. On a public server it must be unique:
     * <prefix>-mqtt, <prefix>-stm32-lwm2m, ... */
    char defid[64];
    bool has_id = http_param(r, "id", v, sizeof(v)) && v[0];
    if (L.public_mode) {
        snprintf(defid, sizeof(defid), "%s-%s%s", L.id_prefix, on_hw ? "stm32-" : "", dev_names[d]);
    } else {
        snprintf(defid, sizeof(defid), "%s", d == DEV_MQTT ? "mqtt-dev-1" : "lwm2m-dev-1");
    }

    argv[a++] = exe;
    for (int i = 0; allowed[d][i]; i++) {
        if (http_param(r, allowed[d][i], v, sizeof(v)) && v[0]) {
            if (!safe_token(v)) {
                snprintf(err, errcap, "bad value for %s", allowed[d][i]);
                return 400;
            }
            snprintf(store[st], sizeof(store[st]), "--%s", allowed[d][i]);
            argv[a++] = store[st++];
            snprintf(store[st], sizeof(store[st]), "%s", v);
            argv[a++] = store[st++];
        }
    }
    if (d == DEV_MQTT) {
        if (http_param(r, "probe", v, sizeof(v)) && strcmp(v, "0") == 0) argv[a++] = "--no-probe";
        if (http_param(r, "tls12", v, sizeof(v)) && strcmp(v, "1") == 0) argv[a++] = "--tls12";
        if (strcmp(L.topic, "playground") != 0) {
            argv[a++] = "--topic";
            argv[a++] = L.topic;
        }
    }
    if (!has_id && L.public_mode && d != DEV_OTA) {
        snprintf(store[st], sizeof(store[st]), "%s", defid);
        argv[a++] = "--id";
        argv[a++] = store[st++];
    }
    if (d == DEV_LWM2M && http_param(r, "notify", v, sizeof(v)) && strcmp(v, "non") == 0) {
        argv[a++] = "--notify-non";
    }

    if (on_hw) {
        /* The board is on the cellular network: it reaches the local servers
         * through the public (tunnel) addresses given with --hw-mqtt etc. */
        static const char *opt[2][2] = { { "--hw-mqtt", "--hw-mqtts" }, { "--hw-coap", "--hw-coaps" } };
        char *host = d == DEV_MQTT ? (secure ? L.hw_mqtts_host : L.hw_mqtt_host)
                                   : (secure ? L.hw_coaps_host : L.hw_coap_host);
        uint16_t port = d == DEV_MQTT ? (secure ? L.hw_mqtts_port : L.hw_mqtt_port)
                                      : (secure ? L.hw_coaps_port : L.hw_coap_port);
        if (!host[0]) {
            snprintf(err, errcap,
                     "the board needs a public address for %s %s - start the lab with %s host:port (your tunnel to "
                     "port %u)",
                     d == DEV_MQTT ? "MQTT" : "LwM2M", secure ? "with security" : "without security",
                     opt[d == DEV_LWM2M][secure],
                     (unsigned)(d == DEV_MQTT ? (secure ? L.mqtts_port : L.mqtt_port)
                                              : (secure ? L.coaps_port : L.coap_port)));
            return 400;
        }
        argv[a++] = "--host";
        argv[a++] = host;
        snprintf(portbuf, sizeof(portbuf), "%u", (unsigned)port);
    } else {
        argv[a++] = "--host";
        argv[a++] = d == DEV_MQTT ? L.mqtt_host : (d == DEV_LWM2M ? L.leshan_host : "127.0.0.1");
        snprintf(portbuf, sizeof(portbuf), "%u",
                 (unsigned)(d == DEV_MQTT ? (secure ? L.mqtts_port : L.mqtt_port)
                            : d == DEV_LWM2M ? (secure ? L.coaps_port : L.coap_port) : L.http_port));
    }
    argv[a++] = "--port";
    argv[a++] = portbuf;
    if (secure && d != DEV_OTA) {
        argv[a++] = "--secure";
        if (d == DEV_MQTT) {
            /* the board has both CAs built in, by file name */
            argv[a++] = "--ca";
            argv[a++] = on_hw ? (L.public_mode ? "public-mqtt-ca.pem" : "ca.crt") : L.ca_file;
            argv[a++] = "--tls-name";
            argv[a++] = on_hw ? L.hw_tls_name : L.mqtt_tls_name;
        }
    }
    if (secure && d == DEV_LWM2M) {
        char id[64];
        snprintf(id, sizeof(id), "%s", defid);
        http_param(r, "id", id, sizeof(id));
        if (leshan_put_psk(id, id, "000102030405060708090a0b0c0d0e0f") != 0) {
            snprintf(err, errcap, "could not register the PSK for '%s' with Leshan at %s:%u", id,
                     L.leshan_host, (unsigned)L.leshan_port);
            return 502;
        }
        lab_event("lwm2m", "psk", "registered DTLS PSK identity '%s' in Leshan's security store", id);
    }
    if (!secure && d == DEV_LWM2M) {
        char id[64];
        snprintf(id, sizeof(id), "%s", defid);
        http_param(r, "id", id, sizeof(id));
        leshan_delete_psk(id);
    }
    if (!on_hw) {
        /* the board's events and commands use its serial port instead */
        snprintf(labaddr, sizeof(labaddr), "127.0.0.1:%u", (unsigned)L.events_port);
        argv[a++] = "--lab";
        argv[a++] = labaddr;
        snprintf(ctlbuf, sizeof(ctlbuf), "%u", (unsigned)dev_ctl[d]);
        argv[a++] = "--ctl-port";
        argv[a++] = ctlbuf;
        argv[a++] = "--results";
        argv[a++] = L.results;
    }
    snprintf(dev[d].run, sizeof(dev[d].run), "%s-%s%llu", dev_names[d], on_hw ? "hw-" : "",
             (unsigned long long)pal_wall_ms());
    argv[a++] = "--run";
    argv[a++] = dev[d].run;
    argv[a] = NULL;

    if (on_hw) {
        char line[1024];
        size_t o = (size_t)snprintf(line, sizeof(line), "run %s", dev_names[d]);
        for (int i = 1; i < a && o < sizeof(line); i++) {
            o += (size_t)snprintf(line + o, sizeof(line) - o, " %s", argv[i]);
        }
        hw_send_time();
        mutex_lock(&L.lock);
        dev[d].hw = true;
        dev[d].hw_exit = 0;
        hw.busy = d;
        dev[d].started = pal_wall_ms();
        free(dev[d].state);
        dev[d].state = NULL;
        mutex_unlock(&L.lock);
        if (hw_send(line) != 0) {
            mutex_lock(&L.lock);
            hw.busy = -1;
            mutex_unlock(&L.lock);
            snprintf(err, errcap, "could not write to the board on %s", L.hw_port);
            return 502;
        }
        lab_event(dev_names[d], "start", "started on the board: %s", line + 4);
        return 200;
    }

    mutex_lock(&L.lock);
    if (proc_spawn(&dev[d].proc, argv) != 0) {
        mutex_unlock(&L.lock);
        snprintf(err, errcap, "could not start %s", exe);
        return 500;
    }
    dev[d].hw = false;
    dev[d].started = pal_wall_ms();
    free(dev[d].state);
    dev[d].state = NULL;
    mutex_unlock(&L.lock);
    {
        char cmd[1024];
        size_t o = 0;
        for (int i = 0; i < a && o < sizeof(cmd); i++) {
            o += (size_t)snprintf(cmd + o, sizeof(cmd) - o, "%s%s", i ? " " : "", i == 0 ? dev_exe[d] : argv[i]);
        }
        lab_event(dev_names[d], "start", "started: %s", cmd);
    }
    return 200;
}

static void dev_stop(int d)
{
    if (dev[d].hw) {
        bool busy;
        mutex_lock(&L.lock);
        busy = hw.busy == d;
        mutex_unlock(&L.lock);
        if (!busy) {
            return;
        }
        hw_send("stop");
        /* DISCONNECT / De-register travel over LTE: allow more time than on the PC */
        for (int i = 0; i < 150; i++) {
            pal_sleep_ms(100);
            mutex_lock(&L.lock);
            busy = hw.busy == d;
            mutex_unlock(&L.lock);
            if (!busy) {
                return;
            }
        }
        hw_send("reset");
        mutex_lock(&L.lock);
        hw.busy = -1;
        mutex_unlock(&L.lock);
        lab_event(dev_names[d], "kill", "the board did not stop in 15 s - reset it");
        return;
    }
    send_ctl(d, "stop");
    /* give it time to DISCONNECT / De-register and print its summary */
    for (int i = 0; i < 50; i++) {
        bool running;
        pal_sleep_ms(100);
        mutex_lock(&L.lock);
        running = dev[d].proc.running && !proc_poll(&dev[d].proc);
        mutex_unlock(&L.lock);
        if (!running) {
            return;
        }
    }
    mutex_lock(&L.lock);
    proc_kill(&dev[d].proc);
    mutex_unlock(&L.lock);
    lab_event(dev_names[d], "kill", "%s did not stop in 5 s - killed", dev_exe[d]);
}

/* ---- device events (UDP) --------------------------------------------------- */

static void append_run(const char *json)
{
    char path[600];
    FILE *f;
    snprintf(path, sizeof(path), "%s/runs.jsonl", L.results);
    pal_mkdir(L.results);
    f = fopen(path, "a");
    if (f) {
        fprintf(f, "%s\n", json);
        fclose(f);
    }
}

/* One JSON object from a device (UDP datagram from a PC process, or a line
 * from the board). */
static void device_json(const char *buf, size_t n, bool from_hw)
{
    if (strncmp(buf, "{\"k\":\"state\"", 12) == 0) {
        const char *kind = strstr(buf, "\"kind\":\"");
        int d = -1;
        if (kind) {
            char k[16] = "";
            sscanf(kind + 8, "%15[^\"]", k);
            d = dev_index(k);
        }
        if (d >= 0) {
            mutex_lock(&L.lock);
            free(dev[d].state);
            dev[d].state = malloc(n + 1);
            if (dev[d].state) memcpy(dev[d].state, buf, n + 1);
            mutex_unlock(&L.lock);
        }
    } else if (strncmp(buf, "{\"k\":\"run\"", 10) == 0) {
        /* store just the summary object, tagged with where it ran */
        const char *sum = strstr(buf, "\"summary\":");
        if (sum && sum[10] == '{') {
            const char *tag = from_hw ? "\"device\":\"stm32-ec200u\"," : "\"device\":\"pc\",";
            char *copy = malloc(strlen(sum) + strlen(tag) + 1);
            if (copy) {
                size_t l;
                snprintf(copy, strlen(sum) + strlen(tag) + 1, "{%s%s", tag, sum + 11);
                l = strlen(copy);
                if (l && copy[l - 1] == '}') copy[l - 1] = '\0'; /* outer object's brace */
                append_run(copy);
                free(copy);
            }
        }
    }
    lab_broadcast(buf);
}

static void event_receiver(void *arg)
{
    static char buf[65536];
    pal_sock_t s;
    (void)arg;
    if (pal_udp_bind(&s, "127.0.0.1", L.events_port) != 0) {
        fprintf(stderr, "[lab] cannot bind UDP 127.0.0.1:%u for device events\n", (unsigned)L.events_port);
        return;
    }
    for (;;) {
        int n = pal_sock_recv(s, buf, sizeof(buf) - 1, 1000);
        if (n <= 0) {
            continue;
        }
        buf[n] = '\0';
        device_json(buf, (size_t)n, false);
    }
}

/* One line from the board: its own status ({"k":"hw"}), a device event
 * (any other JSON), or console text. */
static void hw_line(const char *line, size_t len)
{
    if (strncmp(line, "{\"k\":\"hw\"", 9) == 0) {
        hw_status_line(line);
        lab_broadcast(line);
    } else if (line[0] == '{') {
        device_json(line, len, true);
    } else {
        printf("[board] %s\n", line);
        fflush(stdout);
        if (strncmp(line, "[ec200u]", 8) == 0 || strncmp(line, "[hw]", 4) == 0) {
            lab_event("hw", "console", "%s", line);
        }
    }
}

static void hw_reader(void *arg)
{
    static char line[16384];
    char buf[4096];
    bool reported = false;
    (void)arg;

    for (;;) {
        serial_t *s = serial_open(L.hw_port, L.hw_baud);
        size_t ll = 0;
        if (s == NULL) {
            if (!reported) {
                lab_event("hw", "offline", "board: cannot open %s (not plugged in, or in use by a serial terminal?) - retrying",
                          L.hw_port);
                reported = true;
            }
            pal_sleep_ms(2000);
            continue;
        }
        reported = false;
        mutex_lock(&hw.lock);
        hw.ser = s;
        mutex_unlock(&hw.lock);
        mutex_lock(&L.lock);
        hw.online = true;
        mutex_unlock(&L.lock);
        lab_event("hw", "online", "board link open on %s (%u baud)", L.hw_port, L.hw_baud);
        hw_send_time();
        hw_send("status");

        for (;;) {
            int n = serial_read(s, buf, sizeof(buf), 200);
            if (n < 0) {
                break;
            }
            for (int i = 0; i < n; i++) {
                char c = buf[i];
                if (c == '\n' || c == '\r') {
                    if (ll > 0) {
                        line[ll] = '\0';
                        hw_line(line, ll);
                        ll = 0;
                    }
                } else if (ll + 1 < sizeof(line)) {
                    line[ll++] = c;
                }
            }
        }

        mutex_lock(&hw.lock);
        hw.ser = NULL;
        mutex_unlock(&hw.lock);
        serial_close(s);
        mutex_lock(&L.lock);
        hw.online = false;
        hw.busy = -1;
        snprintf(hw.state, sizeof(hw.state), "offline");
        mutex_unlock(&L.lock);
        lab_event("hw", "offline", "board link on %s lost - retrying", L.hw_port);
        pal_sleep_ms(2000);
    }
}

/* ---- static files, runs, OTA --------------------------------------------- */

static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    char *b;
    long n;
    if (f == NULL) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = malloc((size_t)n + 1);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) {
        free(b);
        b = NULL;
    }
    fclose(f);
    if (b) {
        b[n] = '\0';
        *len = (size_t)n;
    }
    return b;
}

static bool safe_name(const char *p)
{
    return strstr(p, "..") == NULL && strchr(p, '\\') == NULL && strchr(p, ':') == NULL;
}

static void serve_file(pal_sock_t s, const char *root, const char *rel, bool ka)
{
    char path[1100];
    size_t len;
    char *b;
    if (!safe_name(rel)) {
        http_json(s, 400, "{\"error\":\"bad path\"}", ka);
        return;
    }
    snprintf(path, sizeof(path), "%s/%s", root, rel);
    b = slurp(path, &len);
    if (b == NULL) {
        http_json(s, 404, "{\"error\":\"not found\"}", ka);
        return;
    }
    http_respond(s, 200, http_mime(path), b, len, ka);
    free(b);
}

static void serve_runs(pal_sock_t s, bool ka)
{
    char path[600];
    size_t len = 0, o = 1;
    char *b, *out, *line, *next;
    snprintf(path, sizeof(path), "%s/runs.jsonl", L.results);
    b = slurp(path, &len);
    if (b == NULL) {
        http_json(s, 200, "[]", ka);
        return;
    }
    out = malloc(len + 4);
    out[0] = '[';
    for (line = b; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = '\0';
        if (line[0] == '{') {
            if (o > 1) out[o++] = ',';
            memcpy(out + o, line, strlen(line));
            o += strlen(line);
        }
    }
    out[o++] = ']';
    http_respond(s, 200, "application/json", out, o, ka);
    free(out);
    free(b);
}

/* Firmware image by name: a file from the OTA directory, or a synthetic
 * fw_<N>k.bin of N KiB generated on demand for size sweeps. */
static uint8_t *load_image(const char *name, size_t *len)
{
    unsigned kib = 0;
    char tail[8] = "";
    if (!safe_name(name) || strchr(name, '/')) {
        return NULL;
    }
    if (sscanf(name, "fw_%uk%7s", &kib, tail) == 2 && strcmp(tail, ".bin") == 0 && kib >= 1 && kib <= 8192) {
        uint8_t *b = malloc((size_t)kib * 1024);
        if (b) {
            fw_image_fill(b, kib * 1024u, "3.0.0");
            *len = (size_t)kib * 1024;
        }
        return b;
    }
    {
        char path[700];
        snprintf(path, sizeof(path), "%s/%s", L.ota_dir, name);
        return (uint8_t *)slurp(path, len);
    }
}

static void serve_ota(pal_sock_t s, const http_req_t *r, bool ka)
{
    char name[128];
    size_t len;
    uint8_t *img;

    if (strcmp(r->path, "/ota/manifest") == 0) {
        unsigned char hash[32];
        char hex[65], ver[17] = "unknown", json[512];
        if (!http_param(r, "image", name, sizeof(name)) || (img = load_image(name, &len)) == NULL) {
            http_json(s, 404, "{\"error\":\"no such image\"}", ka);
            return;
        }
        mbedtls_sha256(img, len, hash, 0);
        for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", hash[i]);
        if (len >= FW_HEADER_SIZE && memcmp(img, "PGFW", 4) == 0) {
            memcpy(ver, img + 8, 16);
            ver[16] = '\0';
        }
        snprintf(json, sizeof(json),
                 "{\"image\":\"%s\",\"version\":\"%s\",\"size\":%u,\"sha256\":\"%s\",\"simulated\":true}",
                 name, ver, (unsigned)len, hex);
        http_json(s, 200, json, ka);
        free(img);
        return;
    }
    snprintf(name, sizeof(name), "%s", r->path + 5);
    img = load_image(name, &len);
    if (img == NULL) {
        http_json(s, 404, "{\"error\":\"no such image\"}", ka);
        return;
    }
    if (r->range_start >= 0) {
        size_t a = (size_t)r->range_start;
        size_t b = r->range_end >= 0 ? (size_t)r->range_end : len - 1;
        char head[300];
        if (a >= len) {
            http_json(s, 416, "{\"error\":\"range\"}", ka);
            free(img);
            return;
        }
        if (b >= len) b = len - 1;
        snprintf(head, sizeof(head),
                 "HTTP/1.1 206 Partial Content\r\nContent-Type: application/octet-stream\r\n"
                 "Content-Length: %u\r\nContent-Range: bytes %u-%u/%u\r\nConnection: %s\r\n\r\n",
                 (unsigned)(b - a + 1), (unsigned)a, (unsigned)b, (unsigned)len, ka ? "keep-alive" : "close");
        if (pal_sock_send_all(s, head, strlen(head)) == 0) {
            pal_sock_send_all(s, img + a, b - a + 1);
        }
    } else {
        http_respond(s, 200, "application/octet-stream", img, len, ka);
    }
    free(img);
}

/* ---- Leshan proxy ----------------------------------------------------------- */

static void proxy_leshan(pal_sock_t s, http_req_t *r)
{
    upstream_t *up;
    char head[9000], buf[16384];
    size_t o;
    const char *line;

    if ((up = leshan_connect()) == NULL) {
        http_json(s, 502, "{\"error\":\"Leshan is not reachable\"}", false);
        return;
    }
    /* HTTP/1.0 + Connection: close upstream: no chunking, end = close. */
    o = (size_t)snprintf(head, sizeof(head), "%s %s%s%s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n",
                         r->method, r->path + 7, r->query[0] ? "?" : "", r->query, L.leshan_host);
    line = strstr(r->raw_head, "\r\n");
    while (line && line + 2 < r->raw_head + r->head_len - 2) {
        const char *end;
        line += 2;
        end = strstr(line, "\r\n");
        if (end == NULL) break;
        if (strncmp(line, "Host:", 5) && strncmp(line, "host:", 5) && strncmp(line, "Connection:", 11) &&
            strncmp(line, "connection:", 11) && strncmp(line, "Keep-Alive", 10) &&
            strncmp(line, "Accept-Encoding", 15) && strncmp(line, "accept-encoding", 15) &&
            o + (size_t)(end - line) + 4 < sizeof(head)) {
            memcpy(head + o, line, (size_t)(end - line) + 2);
            o += (size_t)(end - line) + 2;
        }
        line = end;
    }
    memcpy(head + o, "\r\n", 2);
    o += 2;
    if (upstream_send_all(up, head, o) != 0 ||
        (r->content_length > 0 && upstream_send_all(up, r->body, (size_t)r->content_length) != 0)) {
        upstream_close(up);
        return;
    }
    for (;;) {
        int n = upstream_recv(up, buf, sizeof(buf), 3600000);
        if (n <= 0 || pal_sock_send_all(s, buf, (size_t)n) != 0) {
            break;
        }
    }
    upstream_close(up);
}

/* ---- request routing ---------------------------------------------------------- */

static void api_state(pal_sock_t s, bool ka)
{
    static char out[65536];
    size_t o;
    o = (size_t)snprintf(out, sizeof(out), "{\"t\":%llu,", (unsigned long long)pal_wall_ms());
    monitor_state_json(out + o, sizeof(out) - o);
    o = strlen(out);
    o += (size_t)snprintf(out + o, sizeof(out) - o, ",\"devices\":{");
    mutex_lock(&L.lock);
    for (int i = 0; i < DEV_COUNT; i++) {
        o += (size_t)snprintf(out + o, sizeof(out) - o,
                              "%s\"%s\":{\"running\":%s,\"hw\":%s,\"run\":\"%s\",\"started\":%llu,\"exit_code\":%d,"
                              "\"state\":%s}",
                              i ? "," : "", dev_names[i], dev_running(i) ? "true" : "false",
                              dev[i].hw ? "true" : "false", dev[i].run, (unsigned long long)dev[i].started,
                              dev[i].hw ? dev[i].hw_exit : dev[i].proc.exit_code, dev[i].state ? dev[i].state : "null");
    }
    {
        char esc[600];
        json_escape(esc, sizeof(esc), hw.msg, sizeof(hw.msg));
        o += (size_t)snprintf(out + o, sizeof(out) - o,
                              "},\"hw\":{\"configured\":%s,\"port\":\"%s\",\"online\":%s,\"state\":\"%s\",\"busy\":\"%s\","
                              "\"msg\":\"%s\",\"status\":%s,\"mqtt\":\"%s:%u\",\"mqtts\":\"%s:%u\",\"coap\":\"%s:%u\","
                              "\"coaps\":\"%s:%u\"",
                              L.hw_port[0] ? "true" : "false", L.hw_port, hw.online ? "true" : "false", hw.state,
                              hw.busy >= 0 ? dev_names[hw.busy] : "", esc, hw.status ? hw.status : "null",
                              L.hw_mqtt_host, (unsigned)L.hw_mqtt_port, L.hw_mqtts_host, (unsigned)L.hw_mqtts_port,
                              L.hw_coap_host, (unsigned)L.hw_coap_port, L.hw_coaps_host, (unsigned)L.hw_coaps_port);
    }
    mutex_unlock(&L.lock);
    snprintf(out + o, sizeof(out) - o,
             "},\"config\":{\"events_port\":%u,\"results\":\"%s\",\"public\":%s,\"topic\":\"%s\",\"id_prefix\":\"%s\","
             "\"tc_netem\":%s}}",
             (unsigned)L.events_port, "results", L.public_mode ? "true" : "false", L.topic, L.id_prefix,
#ifdef __linux__
             "true"
#else
             "false"
#endif
    );
    http_json(s, 200, out, ka);
}

static void handle(pal_sock_t s, http_req_t *r, bool *keep)
{
    char v[256], v2[1024], err[300], json[600];
    bool ka = r->keep_alive;
    bool post = strcmp(r->method, "POST") == 0;
    int d;

    *keep = ka;
    if (strcmp(r->path, "/api/events") == 0) {
        sse_attach(s);
        *keep = false;
        return; /* socket now owned by the hub */
    }
    if (strncmp(r->path, "/leshan/", 8) == 0) {
        proxy_leshan(s, r);
        *keep = false;
        pal_sock_close(s);
        return;
    }
    if (strcmp(r->path, "/api/state") == 0) {
        api_state(s, ka);
    } else if (post && strcmp(r->path, "/api/device/start") == 0) {
        d = dev_index(http_param(r, "kind", v, sizeof(v)));
        if (d < 0) {
            http_json(s, 400, "{\"error\":\"kind must be mqtt, lwm2m or ota\"}", ka);
        } else {
            int st = dev_start(d, r, err, sizeof(err));
            if (st == 200) {
                snprintf(json, sizeof(json), "{\"ok\":true,\"run\":\"%s\"}", dev[d].run);
            } else {
                char esc[400];
                json_escape(esc, sizeof(esc), err, sizeof(err));
                snprintf(json, sizeof(json), "{\"error\":\"%s\"}", esc);
            }
            http_json(s, st, json, ka);
        }
    } else if (post && strcmp(r->path, "/api/device/stop") == 0) {
        d = dev_index(http_param(r, "kind", v, sizeof(v)));
        if (d < 0) {
            http_json(s, 400, "{\"error\":\"unknown kind\"}", ka);
        } else {
            dev_stop(d);
            http_json(s, 200, "{\"ok\":true}", ka);
        }
    } else if (post && strcmp(r->path, "/api/device/cmd") == 0) {
        d = dev_index(http_param(r, "kind", v, sizeof(v)));
        if (d < 0 || !http_param(r, "cmd", v2, sizeof(v2))) {
            http_json(s, 400, "{\"error\":\"kind and cmd required\"}", ka);
        } else {
            send_ctl(d, v2);
            http_json(s, 200, "{\"ok\":true}", ka);
        }
    } else if (post && strcmp(r->path, "/api/mqtt/publish") == 0) {
        char payload[1024];
        if (!http_param(r, "topic", v, sizeof(v)) || !http_param(r, "payload", payload, sizeof(payload))) {
            http_json(s, 400, "{\"error\":\"topic and payload required\"}", ka);
        } else {
            int q = http_param(r, "qos", v2, sizeof(v2)) ? atoi(v2) : 0;
            http_json(s, monitor_publish(v, payload, q == 1 ? 1 : 0) == 0 ? 200 : 502,
                      "{\"ok\":true}", ka);
        }
    } else if (strcmp(r->path, "/api/runs") == 0 && !post) {
        serve_runs(s, ka);
    } else if (post && strcmp(r->path, "/api/runs/reset") == 0) {
        char a[600], b[700];
        snprintf(a, sizeof(a), "%s/runs.jsonl", L.results);
        snprintf(b, sizeof(b), "%s/runs-%llu.jsonl.bak", L.results, (unsigned long long)(pal_wall_ms() / 1000));
        rename(a, b);
        lab_event("lab", "reset", "comparison results archived to %s", b);
        http_json(s, 200, "{\"ok\":true}", ka);
    } else if (post && strcmp(r->path, "/api/reset") == 0) {
        monitor_reset_counters();
        hub_clear();
        http_json(s, 200, "{\"ok\":true}", ka);
    } else if (strncmp(r->path, "/results/", 9) == 0) {
        serve_file(s, L.results, r->path + 9, ka);
    } else if (strncmp(r->path, "/docs/", 6) == 0) {
        serve_file(s, PG_SOURCE_DIR "/docs", r->path + 6, ka);
    } else if (strncmp(r->path, "/ota/", 5) == 0) {
        serve_ota(s, r, ka);
    } else if (strcmp(r->path, "/") == 0) {
        serve_file(s, L.www, "index.html", ka);
    } else if (!post) {
        serve_file(s, L.www, r->path + 1, ka);
    } else {
        http_json(s, 404, "{\"error\":\"not found\"}", ka);
    }
}

static void connection(void *arg)
{
    pal_sock_t s = (pal_sock_t)(intptr_t)arg;
    http_req_t *r = malloc(sizeof(*r));
    bool keep = true;
    while (r && keep) {
        if (http_read_request(s, r) != 0) {
            http_free_request(r);
            break;
        }
        handle(s, r, &keep);
        if (strcmp(r->path, "/api/events") == 0 || strncmp(r->path, "/leshan/", 8) == 0) {
            http_free_request(r);
            free(r);
            return; /* socket handed off or already closed */
        }
        http_free_request(r);
    }
    free(r);
    pal_sock_close(s);
}

/* ---- main ---------------------------------------------------------------------- */

static void usage(void)
{
    printf("lab - MQTT vs LwM2M playground dashboard server\n\n"
           "  --http-port N     dashboard port (default 8000)\n"
           "  --events-port N   UDP port devices report to (default 7000)\n"
           "  --mqtt-host H     broker host (default 127.0.0.1)\n"
           "  --mqtt-port N     broker plain port (default 1883)\n"
           "  --mqtts-port N    broker TLS port (default 8883)\n"
           "  --leshan-host H   Leshan host (default 127.0.0.1)\n"
           "  --leshan-port N   Leshan web/REST port (default 8080)\n"
           "  --coap-port N     Leshan CoAP port (default 5683)\n"
           "  --coaps-port N    Leshan CoAP/DTLS port (default 5684)\n"
           "  --www DIR         dashboard files (default <source>/dashboard/www)\n"
           "  --results DIR     benchmark results (default <source>/results)\n"
           "  --public          use the free public test servers broker.emqx.io and\n"
           "                    leshan.eclipseprojects.io instead of local ones (data is public)\n"
           "  --id-prefix P     name prefix for devices/topics on public servers (default pg-<hash of PC name>)\n"
           "\n"
           "Real board (NUCLEO-L452RE-P + EC200U, see hardware/stm32l452-ec200u/README.md):\n"
           "  --hw-serial PORT  its ST-LINK serial port, e.g. COM10 or /dev/ttyACM0\n"
           "  --hw-baud N       serial speed (default 921600, must match the firmware)\n"
           "  --hw-mqtt H:P     public address that reaches the broker's plain port (tunnel)\n"
           "  --hw-mqtts H:P    ... the broker's TLS port\n"
           "  --hw-coap H:P     ... Leshan's CoAP port (UDP)\n"
           "  --hw-coaps H:P    ... Leshan's DTLS port (UDP)\n"
           "  --hw-tls-name N   name the board checks the broker certificate for (default localhost)\n");
}

static const char *arg(int argc, char **argv, const char *name, const char *def)
{
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], name) == 0) return argv[i + 1];
    }
    return def;
}

/* "host:port" -> host, port. Returns false if the port is missing. */
static bool host_port(const char *v, char *host, size_t cap, uint16_t *port)
{
    const char *colon = v ? strrchr(v, ':') : NULL;
    host[0] = '\0';
    *port = 0;
    if (colon == NULL || colon == v || atoi(colon + 1) <= 0) {
        return v == NULL;
    }
    snprintf(host, cap, "%.*s", (int)(colon - v), v);
    *port = (uint16_t)atoi(colon + 1);
    return true;
}

int main(int argc, char **argv)
{
    pal_sock_t lst;
    char dir[512];

    setvbuf(stdout, NULL, _IONBF, 0);
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage();
            return 0;
        }
    }
    pal_net_init();
    mutex_init(&L.lock);
    mutex_init(&hub.lock);

    snprintf(dir, sizeof(dir), "%s", argv[0]);
    for (char *p = dir; *p; p++) if (*p == '\\') *p = '/';
    {
        char *slash = strrchr(dir, '/');
        if (slash) *slash = '\0'; else snprintf(dir, sizeof(dir), ".");
    }
    snprintf(L.bin_dir, sizeof(L.bin_dir), "%s", dir);
    snprintf(L.bind, sizeof(L.bind), "%s", "127.0.0.1");
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--public") == 0) L.public_mode = true;
    }
    L.http_port = (uint16_t)atoi(arg(argc, argv, "--http-port", "8000"));
    L.events_port = (uint16_t)atoi(arg(argc, argv, "--events-port", "7000"));
    snprintf(L.mqtt_host, sizeof(L.mqtt_host), "%s",
             /* broker.emqx.io rather than test.mosquitto.org: the latter often
              * closes connections arriving through mobile-operator CGNAT */
             arg(argc, argv, "--mqtt-host", L.public_mode ? "broker.emqx.io" : "127.0.0.1"));
    L.mqtt_port = (uint16_t)atoi(arg(argc, argv, "--mqtt-port", "1883"));
    L.mqtts_port = (uint16_t)atoi(arg(argc, argv, "--mqtts-port", "8883"));
    snprintf(L.leshan_host, sizeof(L.leshan_host), "%s",
             arg(argc, argv, "--leshan-host", L.public_mode ? "leshan.eclipseprojects.io" : "127.0.0.1"));
    L.leshan_port = (uint16_t)atoi(arg(argc, argv, "--leshan-port", L.public_mode ? "443" : "8080"));
    L.leshan_tls = L.leshan_port == 443;
    snprintf(L.leshan_ca, sizeof(L.leshan_ca), "%s", PG_SOURCE_DIR "/servers/public/letsencrypt-roots.pem");
    L.coap_port = (uint16_t)atoi(arg(argc, argv, "--coap-port", "5683"));
    L.coaps_port = (uint16_t)atoi(arg(argc, argv, "--coaps-port", "5684"));
    snprintf(L.www, sizeof(L.www), "%s", arg(argc, argv, "--www", PG_SOURCE_DIR "/dashboard/www"));
    snprintf(L.results, sizeof(L.results), "%s", arg(argc, argv, "--results", PG_SOURCE_DIR "/results"));
    snprintf(L.ota_dir, sizeof(L.ota_dir), "%s/ota", L.bin_dir);
    snprintf(L.ca_file, sizeof(L.ca_file), "%s",
             L.public_mode ? PG_SOURCE_DIR "/servers/public/public-mqtt-ca.pem" /* EMQX + test.mosquitto.org */
                           : PG_SOURCE_DIR "/servers/mosquitto/certs/ca.crt");
    snprintf(L.mqtt_tls_name, sizeof(L.mqtt_tls_name), "%s", L.public_mode ? L.mqtt_host : "localhost");
    if (L.public_mode) {
        /* A stable name for this PC, so its devices and topics don't mix with
         * everyone else's on the shared servers. */
        const char *who = getenv("COMPUTERNAME");
        uint32_t h = 2166136261u;
        if (who == NULL) who = getenv("HOSTNAME");
        for (const char *p = who ? who : "playground"; *p; p++) {
            h = (h ^ (uint8_t)*p) * 16777619u;
        }
        snprintf(L.id_prefix, sizeof(L.id_prefix), "%s", arg(argc, argv, "--id-prefix", ""));
        if (!L.id_prefix[0]) {
            snprintf(L.id_prefix, sizeof(L.id_prefix), "pg-%06x", (unsigned)(h & 0xffffffu));
        }
        snprintf(L.topic, sizeof(L.topic), "playground/%s", L.id_prefix);
    } else {
        snprintf(L.topic, sizeof(L.topic), "playground");
    }
    upstream_init();
    pal_mkdir(L.results);

    mutex_init(&hw.lock);
    snprintf(L.hw_port, sizeof(L.hw_port), "%s", arg(argc, argv, "--hw-serial", ""));
    L.hw_baud = (unsigned)atoi(arg(argc, argv, "--hw-baud", "921600"));
    snprintf(L.hw_tls_name, sizeof(L.hw_tls_name), "%s", arg(argc, argv, "--hw-tls-name", L.mqtt_tls_name));
    if (!host_port(arg(argc, argv, "--hw-mqtt", NULL), L.hw_mqtt_host, sizeof(L.hw_mqtt_host), &L.hw_mqtt_port) ||
        !host_port(arg(argc, argv, "--hw-mqtts", NULL), L.hw_mqtts_host, sizeof(L.hw_mqtts_host), &L.hw_mqtts_port) ||
        !host_port(arg(argc, argv, "--hw-coap", NULL), L.hw_coap_host, sizeof(L.hw_coap_host), &L.hw_coap_port) ||
        !host_port(arg(argc, argv, "--hw-coaps", NULL), L.hw_coaps_host, sizeof(L.hw_coaps_host), &L.hw_coaps_port)) {
        fprintf(stderr, "[lab] --hw-mqtt/--hw-mqtts/--hw-coap/--hw-coaps take host:port\n");
        return 2;
    }
    if (L.public_mode) {
        /* Public servers are on the Internet: the board connects to them directly. */
        if (!L.hw_mqtt_host[0]) { snprintf(L.hw_mqtt_host, sizeof(L.hw_mqtt_host), "%s", L.mqtt_host); L.hw_mqtt_port = L.mqtt_port; }
        if (!L.hw_mqtts_host[0]) { snprintf(L.hw_mqtts_host, sizeof(L.hw_mqtts_host), "%s", L.mqtt_host); L.hw_mqtts_port = L.mqtts_port; }
        if (!L.hw_coap_host[0]) { snprintf(L.hw_coap_host, sizeof(L.hw_coap_host), "%s", L.leshan_host); L.hw_coap_port = L.coap_port; }
        if (!L.hw_coaps_host[0]) { snprintf(L.hw_coaps_host, sizeof(L.hw_coaps_host), "%s", L.leshan_host); L.hw_coaps_port = L.coaps_port; }
    }

    if (pal_tcp_listen(&lst, L.bind, L.http_port) != 0) {
        fprintf(stderr, "[lab] cannot listen on %s:%u - is another lab running? (--http-port)\n", L.bind,
                (unsigned)L.http_port);
        return 1;
    }
    thread_start(event_receiver, NULL);
    thread_start(reaper, NULL);
    thread_start(heartbeat, NULL);
    monitor_start();

    printf("\n  MQTT vs LwM2M playground\n\n");
    printf("  Dashboard:  http://%s:%u\n", L.bind, (unsigned)L.http_port);
    printf("  Broker:     mqtt://%s:%u  (TLS %u)\n", L.mqtt_host, (unsigned)L.mqtt_port, (unsigned)L.mqtts_port);
    printf("  Leshan:     %s://%s:%u  (CoAP %u, DTLS %u)\n", L.leshan_tls ? "https" : "http", L.leshan_host,
           (unsigned)L.leshan_port, (unsigned)L.coap_port, (unsigned)L.coaps_port);
    if (L.public_mode) {
        printf("  Public:     shared test servers - your devices are %s-*, topics %s/... (visible to anyone)\n",
               L.id_prefix, L.topic);
    }
    printf("  Devices:    %s   Results: %s\n", L.bin_dir, L.results);
    if (L.hw_port[0]) {
        printf("  Board:      %s @ %u baud; reaches MQTT %s:%u / TLS %s:%u, CoAP %s:%u / DTLS %s:%u\n", L.hw_port,
               L.hw_baud, L.hw_mqtt_host[0] ? L.hw_mqtt_host : "(unset)", (unsigned)L.hw_mqtt_port,
               L.hw_mqtts_host[0] ? L.hw_mqtts_host : "(unset)", (unsigned)L.hw_mqtts_port,
               L.hw_coap_host[0] ? L.hw_coap_host : "(unset)", (unsigned)L.hw_coap_port,
               L.hw_coaps_host[0] ? L.hw_coaps_host : "(unset)", (unsigned)L.hw_coaps_port);
        thread_start(hw_reader, NULL);
    }
    printf("\n");

    for (;;) {
        pal_sock_t c = pal_tcp_accept(lst);
        if (c == PAL_SOCK_INVALID) {
            continue;
        }
        if (thread_start(connection, (void *)(intptr_t)c) != 0) {
            pal_sock_close(c);
        }
    }
}
