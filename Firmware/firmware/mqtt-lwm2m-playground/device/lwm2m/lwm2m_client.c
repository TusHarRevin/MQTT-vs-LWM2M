#include "lwm2m_client.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "coap_trace.h"
#include "events.h"
#include "liblwm2m.h"
#include "objects.h"
#include "pal.h"

/* Sizes can be lowered for a microcontroller build (-D...). */
#ifndef MAX_PENDING
#define MAX_PENDING 64          /* confirmable notifications in flight */
#endif
#ifndef PENDING_BUF_SIZE
#define PENDING_BUF_SIZE 2048   /* copy of each one, for retransmission */
#endif
#ifndef LWC_RX_BUF_SIZE
#define LWC_RX_BUF_SIZE 4096
#endif
#define MAX_RETRANSMIT 4

typedef struct
{
    bool used;
    uint16_t mid;
    uint8_t buf[PENDING_BUF_SIZE];
    size_t len;
    uint64_t first_us, next_us, wall_ms;
    uint32_t timeout_ms;
    int retries;
    uint32_t tx_bytes;
    uint32_t payload;
    char path[48];
} pending_t;

static struct {
    lwc_cfg_t cfg;
    lwm2m_context_t *ctx;
    lwm2m_object_t **objs;
    link_t link;
    bool link_up;
    coap_tracker_t trk;
    pending_t pend[MAX_PENDING];
    lwc_session_t sess;
    uint64_t start_us;
    bool registered;
    pal_rng_t rng;
    bool link_lost;          /* the socket died under us; reopen it */
    uint64_t next_reopen_us;
    uint8_t rx[LWC_RX_BUF_SIZE];
} c;

/* ---- tracing ------------------------------------------------------------- */

static void observed_path_for_token(const coap_info_t *ci, char *out, size_t cap)
{
    out[0] = '\0';
    if (c.ctx == NULL) {
        return;
    }
    for (lwm2m_observed_t *o = c.ctx->observedList; o; o = o->next) {
        for (lwm2m_watcher_t *w = o->watcherList; w; w = w->next) {
            if (w->tokenLen == ci->tkl && memcmp(w->token, ci->token, ci->tkl) == 0) {
                int n = snprintf(out, cap, "/%u", o->uri.objectId);
                if (LWM2M_URI_IS_SET_INSTANCE(&o->uri)) {
                    n += snprintf(out + n, cap - (size_t)n, "/%u", o->uri.instanceId);
                }
                if (LWM2M_URI_IS_SET_RESOURCE(&o->uri)) {
                    snprintf(out + n, cap - (size_t)n, "/%u", o->uri.resourceId);
                }
                return;
            }
        }
    }
}

/* Human value of a path the device knows, for the event log. */
static void describe_value(const char *path, char *out, size_t cap)
{
    if (strcmp(path, "/3303/0/5700") == 0) snprintf(out, cap, "temperature=%.2f", g_lw.now.temperature);
    else if (strcmp(path, "/3304/0/5700") == 0) snprintf(out, cap, "humidity=%.2f", g_lw.now.humidity);
    else if (strcmp(path, "/3315/0/5700") == 0) snprintf(out, cap, "pressure=%.2f", g_lw.now.pressure);
    else if (strcmp(path, "/3/0/9") == 0) snprintf(out, cap, "battery=%u", g_lw.now.battery);
    else if (strncmp(path, "/19/0", 5) == 0) snprintf(out, cap, "telemetry blob %u B", (unsigned)g_lw.blob_len);
    else snprintf(out, cap, "%s", path);
}

static bool printable_format(int cf)
{
    return cf == 0 || cf == 40 || cf == 110 || cf == 1543 || cf == 11543 || cf == 50;
}

/* Logs one CoAP message. Returns the tracker's send count (>1 = retransmit). */
static int trace(const uint8_t *buf, size_t len, bool from_device, uint64_t t_us, uint32_t wire_len,
                 char *path_out, size_t path_cap, double *lat_out)
{
    coap_info_t ci;
    char name[48], detail[200], code[8], extra[160], path[48] = "", val[64] = "";
    char text[620];
    double lat = -1;
    int sends;
    ev_t e = EV_INIT;

    if (coap_parse(buf, len, &ci) != 0) {
        ev_log("LWM2M", "%s %u bytes that are not CoAP", from_device ? "sent" : "received",
               (unsigned)len);
        return 0;
    }
    sends = coap_track(&c.trk, &ci, from_device, t_us, name, sizeof(name), &lat);
    coap_code_str(ci.code, code);
    if (strcmp(name, "NOTIFY") == 0 || ci.has_observe) {
        observed_path_for_token(&ci, path, sizeof(path));
    }
    if (strcmp(name, "NOTIFY") == 0 && path[0]) {
        describe_value(path, val, sizeof(val));
    }
    snprintf(detail, sizeof(detail), "%s %s %s%s%s%s", coap_type_name(ci.type), code,
             ci.code < 32 && ci.code ? ci.uri : (path[0] ? path : ""), val[0] ? " " : "", val,
             sends > 1 ? " (retransmission)" : "");
    snprintf(extra, sizeof(extra),
             "\"mid\":%u,\"ctype\":\"%s\",\"code\":\"%s\",\"uri\":\"%s\",\"cf\":\"%s\",\"wire\":%u%s",
             (unsigned)ci.mid, coap_type_name(ci.type), code, path[0] ? path : ci.uri,
             ci.content_format >= 0 ? coap_format_name(ci.content_format) : "", (unsigned)wire_len,
             sends > 1 ? ",\"retransmit\":true" : "");
    if (ci.payload_len && printable_format(ci.content_format)) {
        snprintf(text, sizeof(text), "%.*s", (int)(ci.payload_len > 600 ? 600 : ci.payload_len),
                 (const char *)buf + ci.payload_off);
        e.payload = text;
    }
    e.proto = "LWM2M";
    e.from = from_device ? "DEVICE" : "SERVER";
    e.to = from_device ? "SERVER" : "DEVICE";
    e.type = name;
    e.len = (int)len;
    e.payload_len = (int)ci.payload_len;
    e.lat_ms = lat;
    e.detail = detail;
    e.raw = buf;
    e.raw_len = len;
    e.extra = extra;
    ev_emit(&e);
    if (!from_device && ci.code == 131 && strncmp(name, "REGISTER", 8) == 0) { /* 4.03 */
        ev_log("LWM2M", "server refused registration (4.03 Forbidden). Leshan has security (PSK) info for "
                        "this endpoint, so it only accepts it over DTLS: run with --secure, or delete the "
                        "entry in Leshan's Security tab.");
    }
    if (path_out) {
        snprintf(path_out, path_cap, "%s", path);
    }
    if (lat_out) {
        *lat_out = lat;
    }
    return sends;
}

/* ---- confirmable notifications ------------------------------------------ */

static void pending_finish(pending_t *p, bool ok, double lat, uint32_t rx_bytes)
{
    lwc_notify_t n;
    memset(&n, 0, sizeof(n));
    snprintf(n.path, sizeof(n.path), "%s", p->path);
    n.mid = p->mid;
    n.t_ms = p->wall_ms;
    n.ok = ok;
    n.lat_ms = ok ? lat : -1;
    n.retries = p->retries;
    n.tx_bytes = p->tx_bytes;
    n.rx_bytes = rx_bytes;
    n.payload = p->payload;
    p->used = false;
    if (!ok) {
        ev_log("LWM2M", "NOTIFY mid=%u %s gave up after %d retransmissions", (unsigned)p->mid,
               p->path, p->retries);
    }
    if (c.cfg.on_notify) {
        c.cfg.on_notify(&n);
    }
}

static void pending_tick(void)
{
    uint64_t now = pal_now_us();
    for (int i = 0; i < MAX_PENDING; i++) {
        pending_t *p = &c.pend[i];
        if (!p->used || now < p->next_us) {
            continue;
        }
        if (p->retries >= MAX_RETRANSMIT || !c.link_up) {
            pending_finish(p, false, -1, 0);
            continue;
        }
        p->retries++;
        p->timeout_ms *= 2;
        p->next_us = now + (uint64_t)p->timeout_ms * 1000ULL;
        {
            uint64_t tx0 = c.link.st.tx_bytes, t0 = pal_now_us();
            if (link_send(&c.link, p->buf, p->len) < 0) {
                continue;
            }
            p->tx_bytes += (uint32_t)(c.link.st.tx_bytes - tx0);
            trace(p->buf, p->len, true, t0, (uint32_t)(c.link.st.tx_bytes - tx0), NULL, 0, NULL);
        }
    }
}

/* An ACK/RST arrived: does it settle one of our notifications? */
static void pending_match(const uint8_t *buf, size_t len, uint32_t wire, double lat)
{
    coap_info_t ci;
    if (coap_parse(buf, len, &ci) != 0 || (ci.type != COAP_ACK && ci.type != COAP_RST)) {
        return;
    }
    for (int i = 0; i < MAX_PENDING; i++) {
        pending_t *p = &c.pend[i];
        if (p->used && p->mid == ci.mid) {
            lat = pal_ms_since(p->first_us); /* from the first transmission */
            /* RST = the server no longer wants this observation */
            pending_finish(p, ci.type == COAP_ACK, lat, wire);
            return;
        }
    }
}

/* ---- Wakaama transport callbacks ----------------------------------------- */

void *lwm2m_connect_server(uint16_t secObjInstID, void *userData)
{
    link_security_t sec = { 0 };
    char detail[200];
    (void)secObjInstID; (void)userData;

    if (c.link_up) {
        return &c.link;
    }
    sec.enabled = c.cfg.secure;
    sec.psk_identity = c.cfg.psk_identity;
    sec.psk = c.cfg.psk;
    sec.psk_len = c.cfg.psk_len;
    if (link_open(&c.link, LINK_UDP, c.cfg.host, c.cfg.port, &sec, c.cfg.sim,
                  c.cfg.dtls_timeout_ms) != 0) {
        ev_log("NET", "%s", c.link.err);
        return NULL;
    }
    c.link_up = true;
    {
        ev_t e = EV_INIT;
        e.proto = "NET";
        e.to = "SERVER";
        e.type = "UDP SOCKET";
        e.lat_ms = c.link.st.connect_ms;
        snprintf(detail, sizeof(detail), "%s:%u (UDP is connectionless - nothing is sent yet)",
                 c.cfg.host, (unsigned)c.cfg.port);
        e.detail = detail;
        ev_emit(&e);
    }
    if (c.cfg.secure) {
        ev_t e = EV_INIT;
        char extra[160];
        e.proto = "DTLS";
        e.to = "SERVER";
        e.type = "HANDSHAKE";
        e.lat_ms = c.link.st.handshake_ms;
        e.len = (int)(c.link.st.hs_tx_bytes + c.link.st.hs_rx_bytes);
        snprintf(detail, sizeof(detail), "%s %s, PSK identity '%s', sent %llu B, received %llu B",
                 c.link.st.security, c.link.st.cipher, c.cfg.psk_identity,
                 (unsigned long long)c.link.st.hs_tx_bytes, (unsigned long long)c.link.st.hs_rx_bytes);
        e.detail = detail;
        snprintf(extra, sizeof(extra), "\"hs_tx\":%llu,\"hs_rx\":%llu,\"cipher\":\"%s\"",
                 (unsigned long long)c.link.st.hs_tx_bytes, (unsigned long long)c.link.st.hs_rx_bytes,
                 c.link.st.cipher);
        e.extra = extra;
        ev_set_security(c.link.st.security);
        ev_emit(&e);
    }
    if (!c.sess.have_session) {
        c.sess.connect_ms = c.link.st.connect_ms;
        c.sess.handshake_ms = c.cfg.secure ? c.link.st.handshake_ms : -1.0;
        c.sess.hs_bytes = c.link.st.hs_tx_bytes + c.link.st.hs_rx_bytes;
        snprintf(c.sess.security, sizeof(c.sess.security), "%s", c.link.st.security);
    }
    return &c.link;
}

void lwm2m_close_connection(void *sessionH, void *userData)
{
    (void)sessionH; (void)userData;
    if (c.link_up) {
        link_close(&c.link);
        c.link_up = false;
    }
    c.link_lost = false; /* closed on purpose */
}

/* The socket failed (e.g. a cellular modem restarted, or the network changed):
 * open it again, DTLS handshake included. Wakaama keeps &c.link as its
 * session handle, so the LwM2M session simply carries on. At most every 2 s. */
static bool reopen_link(void)
{
    uint64_t now = pal_now_us();
    if (c.link_up) {
        ev_log("NET", "socket lost (%s) - reopening", c.link.err[0] ? c.link.err : "receive failed");
        link_close(&c.link);
        c.link_up = false;
    }
    c.link_lost = true;
    if (now < c.next_reopen_us) {
        return false;
    }
    c.next_reopen_us = now + 2000000ULL;
    c.link_lost = lwm2m_connect_server(0, NULL) == NULL;
    return !c.link_lost;
}

uint8_t lwm2m_buffer_send(void *sessionH, uint8_t *buffer, size_t length, void *userData)
{
    coap_info_t ci;
    uint64_t tx0, t0;
    pending_t *p = NULL;
    (void)userData;

    if (sessionH != NULL && !c.link_up && c.link_lost) {
        reopen_link();
    }
    if (sessionH == NULL || !c.link_up) {
        return COAP_500_INTERNAL_SERVER_ERROR;
    }
    /* Notification? (NON 2.05 with Observe) -> make it confirmable. */
    if (c.cfg.notify_con && coap_parse(buffer, length, &ci) == 0 && ci.type == COAP_NON &&
        ci.code == 69 && ci.has_observe && length <= sizeof(c.pend[0].buf)) {
        for (int i = 0; i < MAX_PENDING; i++) {
            if (!c.pend[i].used) {
                p = &c.pend[i];
                break;
            }
        }
        if (p != NULL) {
            buffer[0] = (uint8_t)(buffer[0] & ~0x30); /* type bits := CON (0) */
            memset(p, 0, sizeof(*p));
            p->used = true;
            p->mid = ci.mid;
            memcpy(p->buf, buffer, length);
            p->len = length;
            p->payload = (uint32_t)ci.payload_len;
            p->first_us = pal_now_us();
            p->wall_ms = pal_wall_ms();
            /* RFC 7252: initial timeout random in [ACK_TIMEOUT, ACK_TIMEOUT * 1.5] */
            p->timeout_ms = c.cfg.ack_timeout_ms + (uint32_t)(pal_rng_unit(&c.rng) * c.cfg.ack_timeout_ms / 2);
            p->next_us = p->first_us + (uint64_t)p->timeout_ms * 1000ULL;
        }
    }
    /* Timestamp before link_send: the simulated network delay happens inside it,
     * and it belongs to the round trip (the device handed the message over now). */
    tx0 = c.link.st.tx_bytes;
    t0 = pal_now_us();
    if (link_send(&c.link, buffer, length) < 0) {
        ev_log("NET", "send failed: %s", c.link.err);
        return COAP_500_INTERNAL_SERVER_ERROR;
    }
    {
        uint32_t wire = (uint32_t)(c.link.st.tx_bytes - tx0);
        char path[48];
        trace(buffer, length, true, t0, wire, path, sizeof(path), NULL);
        if (p != NULL) {
            p->tx_bytes = wire;
            snprintf(p->path, sizeof(p->path), "%s", path);
        } else if (!c.cfg.notify_con && coap_parse(buffer, length, &ci) == 0 && ci.type == COAP_NON &&
                   ci.code == 69 && ci.has_observe && c.cfg.on_notify) {
            /* NON notification: delivered or not, we can't know */
            lwc_notify_t n;
            memset(&n, 0, sizeof(n));
            snprintf(n.path, sizeof(n.path), "%s", path);
            n.mid = ci.mid;
            n.t_ms = pal_wall_ms();
            n.ok = true;
            n.lat_ms = -1;
            n.tx_bytes = wire;
            n.payload = (uint32_t)ci.payload_len;
            c.cfg.on_notify(&n);
        }
    }
    return COAP_NO_ERROR;
}

bool lwm2m_session_is_equal(void *s1, void *s2, void *userData)
{
    (void)userData;
    return s1 == s2;
}

/* ---- Wakaama platform callbacks ------------------------------------------ */

void *lwm2m_malloc(size_t s) { return malloc(s); }
void lwm2m_free(void *p) { free(p); }

char *lwm2m_strdup(const char *str)
{
    size_t n = strlen(str) + 1;
    char *d = malloc(n);
    if (d) {
        memcpy(d, str, n);
    }
    return d;
}

int lwm2m_strncmp(const char *s1, const char *s2, size_t n) { return strncmp(s1, s2, n); }

int lwm2m_strcasecmp(const char *a, const char *b)
{
    for (;; a++, b++) {
        int ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        int cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (ca != cb || ca == 0) {
            return ca - cb;
        }
    }
}

time_t lwm2m_gettime(void) { return (time_t)(pal_now_us() / 1000000ULL); }
int lwm2m_seed(void) { return (int)(pal_now_us() & 0x7fffffff); }

void lwm2m_printf(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    vfprintf(stderr, format, ap);
    va_end(ap);
}

/* ---- Public API ---------------------------------------------------------- */

int lwc_start(const lwc_cfg_t *cfg)
{
    int rc;

    c.cfg = *cfg;
    c.registered = false;
    c.link_lost = false;
    c.next_reopen_us = 0;
    memset(&c.trk, 0, sizeof(c.trk));
    memset(c.pend, 0, sizeof(c.pend));
    pal_rng_seed(&c.rng, pal_now_us());

    snprintf(g_lw.server_uri, sizeof(g_lw.server_uri), "%s://%s:%u", cfg->secure ? "coaps" : "coap",
             cfg->host, (unsigned)cfg->port);
    g_lw.psk_mode = cfg->secure;
    snprintf(g_lw.psk_identity, sizeof(g_lw.psk_identity), "%s", cfg->psk_identity ? cfg->psk_identity : "");
    if (cfg->psk && cfg->psk_len <= sizeof(g_lw.psk)) {
        memcpy(g_lw.psk, cfg->psk, cfg->psk_len);
        g_lw.psk_len = cfg->psk_len;
    }
    g_lw.short_id = 123;
    g_lw.lifetime_s = cfg->lifetime_s;

    if (c.objs == NULL) {
        c.objs = objects_create();
        if (c.objs == NULL) {
            return -1;
        }
    }
    c.ctx = lwm2m_init(NULL);
    if (c.ctx == NULL) {
        return -1;
    }
    rc = lwm2m_configure(c.ctx, cfg->endpoint, NULL, NULL, LW_OBJECT_COUNT, c.objs);
    if (rc != 0) {
        ev_log("LWM2M", "lwm2m_configure failed (0x%02x)", rc);
        lwm2m_close(c.ctx);
        c.ctx = NULL;
        return -1;
    }
    c.start_us = pal_now_us();
    return 0;
}

bool lwc_running(void) { return c.ctx != NULL; }
bool lwc_registered(void) { return c.registered; }
const lwc_session_t *lwc_session(void) { return &c.sess; }
const link_t *lwc_link(void) { return &c.link; }

/* The server's status flips to REGISTERED as soon as Wakaama handles the
 * 2.01 response, so check right after each packet: "time to registered" then
 * doesn't include the main loop's polling granularity. */
static void check_registered(void)
{
    bool reg = c.ctx != NULL && c.ctx->serverList != NULL &&
               c.ctx->serverList->status == STATE_REGISTERED;
    if (reg && !c.registered) {
        double ms = pal_ms_since(c.start_us);
        if (!c.sess.have_session) {
            c.sess.register_ms = ms;
            c.sess.have_session = true;
        }
        ev_log("LWM2M", "registered with the server %.1f ms after start", ms);
    } else if (!reg && c.registered) {
        ev_log("LWM2M", "no longer registered");
    }
    c.registered = reg;
}

static void handle_datagram(const uint8_t *buf, size_t n, uint32_t wire)
{
    double lat = -1;
    trace(buf, n, false, pal_now_us(), wire, NULL, 0, &lat);
    pending_match(buf, n, wire, lat);
    if (c.ctx != NULL) {
        lwm2m_handle_packet(c.ctx, (uint8_t *)buf, n, &c.link);
        check_registered();
    }
}

int lwc_step(uint32_t max_wait_ms)
{
    time_t tv = 5;
    uint64_t deadline = pal_now_us() + (uint64_t)max_wait_ms * 1000ULL;
    int rc;

    if (c.ctx == NULL) {
        pal_sleep_ms(max_wait_ms);
        return -1;
    }
    rc = lwm2m_step(c.ctx, &tv);
    if (rc != 0 && rc != COAP_503_SERVICE_UNAVAILABLE) {
        ev_log("LWM2M", "lwm2m_step error 0x%02x", rc);
    }
    check_registered();
    pending_tick();

    while (c.link_up) {
        uint64_t now = pal_now_us();
        uint32_t wait = now >= deadline ? 0 : (uint32_t)((deadline - now) / 1000);
        uint64_t rx0 = c.link.st.rx_bytes;
        int n;
        if ((time_t)wait > tv * 1000) {
            wait = (uint32_t)tv * 1000;
        }
        n = link_recv(&c.link, c.rx, sizeof(c.rx), wait);
        if (n > 0) {
            handle_datagram(c.rx, (size_t)n, (uint32_t)(c.link.st.rx_bytes - rx0));
            continue; /* drain anything else that's queued */
        }
        if (n < 0) {
            c.link_lost = true;
            reopen_link();
        }
        break;
    }
    if (!c.link_up && c.link_lost) {
        reopen_link();
    }
    if (!c.link_up) {
        uint64_t now = pal_now_us();
        if (now < deadline) {
            pal_sleep_ms((uint32_t)((deadline - now) / 1000));
        }
    }
    return 0;
}

void lwc_value_changed(const char *path)
{
    lwm2m_uri_t uri;
    if (c.ctx != NULL && lwm2m_stringToUri(path, strlen(path), &uri) != 0) {
        lwm2m_resource_value_changed(c.ctx, &uri);
    }
}

void lwc_trigger_update(void)
{
    if (c.ctx != NULL) {
        lwm2m_update_registration(c.ctx, 0, false);
    }
}

void lwc_observed(char *out, size_t cap)
{
    size_t o = 0;
    out[0] = '\0';
    if (c.ctx == NULL) {
        return;
    }
    for (lwm2m_observed_t *ob = c.ctx->observedList; ob && o + 24 < cap; ob = ob->next) {
        o += (size_t)snprintf(out + o, cap - o, "%s/%u", o ? "," : "", ob->uri.objectId);
        if (LWM2M_URI_IS_SET_INSTANCE(&ob->uri)) {
            o += (size_t)snprintf(out + o, cap - o, "/%u", ob->uri.instanceId);
        }
        if (LWM2M_URI_IS_SET_RESOURCE(&ob->uri)) {
            o += (size_t)snprintf(out + o, cap - o, "/%u", ob->uri.resourceId);
        }
    }
}

uint32_t lwc_pending_notifies(void)
{
    uint32_t n = 0;
    for (int i = 0; i < MAX_PENDING; i++) {
        n += c.pend[i].used;
    }
    return n;
}

void lwc_deregister(void)
{
    uint64_t until;
    if (c.ctx == NULL) {
        return;
    }
    /* lwm2m_deregister sends the CoAP DELETE; give the ACK a moment to arrive
     * so it shows up in the log, then tear everything down. */
    lwm2m_deregister(c.ctx);
    until = pal_now_us() + 2000000ULL;
    while (c.link_up && pal_now_us() < until) {
        uint64_t rx0 = c.link.st.rx_bytes;
        int n = link_recv(&c.link, c.rx, sizeof(c.rx), 100);
        if (n > 0) {
            coap_info_t ci;
            handle_datagram(c.rx, (size_t)n, (uint32_t)(c.link.st.rx_bytes - rx0));
            if (coap_parse(c.rx, (size_t)n, &ci) == 0 && ci.code == 66) { /* 2.02 Deleted */
                break;
            }
        }
    }
    lwc_stop();
}

void lwc_stop(void)
{
    for (int i = 0; i < MAX_PENDING; i++) {
        if (c.pend[i].used) {
            pending_finish(&c.pend[i], false, -1, 0);
        }
    }
    if (c.ctx != NULL) {
        lwm2m_context_t *ctx = c.ctx;
        c.ctx = NULL;
        /* Server already deregistered or unreachable: lwm2m_close would try again,
         * harmless; it also closes the connection through our callback. */
        lwm2m_close(ctx);
    }
    lwm2m_close_connection(&c.link, NULL);
    c.registered = false;
}
