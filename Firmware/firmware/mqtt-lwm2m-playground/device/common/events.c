#include "events.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "pal.h"

#define EV_MAX_PAYLOAD 600
#define EV_MAX_RAW 192
#ifndef EV_JSON_MAX
#define EV_JSON_MAX 8192 /* largest ev_json() object (run summary); lowered on the STM32 */
#endif

static struct {
    char dev[48];
    char kind[16];
    char lab_ip[48];
    uint16_t lab_port;
    pal_sock_t sock;
    char security[32];
    char run[48];
    unsigned seq;
} g = { .sock = PAL_SOCK_INVALID, .security = "none" };

size_t json_escape(char *dst, size_t cap, const char *src, size_t max_src)
{
    size_t o = 0;
    if (cap == 0) {
        return 0;
    }
    for (size_t i = 0; src != NULL && src[i] != '\0' && i < max_src; i++) {
        unsigned char c = (unsigned char)src[i];
        char tmp[8];
        const char *rep = NULL;
        size_t rl;
        switch (c) {
        case '"': rep = "\\\""; break;
        case '\\': rep = "\\\\"; break;
        case '\n': rep = "\\n"; break;
        case '\r': rep = "\\r"; break;
        case '\t': rep = "\\t"; break;
        default:
            if (c < 0x20 || c >= 0x7f) {
                snprintf(tmp, sizeof(tmp), "\\u%04x", c);
                rep = tmp;
            }
        }
        if (rep == NULL) {
            if (o + 1 >= cap) {
                break;
            }
            dst[o++] = (char)c;
            continue;
        }
        rl = strlen(rep);
        if (o + rl >= cap) {
            break;
        }
        memcpy(dst + o, rep, rl);
        o += rl;
    }
    dst[o] = '\0';
    return o;
}

void ev_init(const char *device_id, const char *kind, const char *lab_ip, uint16_t lab_port)
{
    snprintf(g.dev, sizeof(g.dev), "%s", device_id);
    snprintf(g.kind, sizeof(g.kind), "%s", kind);
    snprintf(g.lab_ip, sizeof(g.lab_ip), "%s", lab_ip ? lab_ip : "127.0.0.1");
    g.lab_port = lab_port;
    if (lab_port != 0 && g.sock == PAL_SOCK_INVALID) {
        /* Ephemeral local port; we only ever send. */
        if (pal_udp_bind(&g.sock, "127.0.0.1", 0) != 0) {
            g.sock = PAL_SOCK_INVALID;
        }
    }
}

void ev_set_security(const char *label)
{
    snprintf(g.security, sizeof(g.security), "%s", label ? label : "none");
}

void ev_set_run(const char *run_id)
{
    snprintf(g.run, sizeof(g.run), "%s", run_id ? run_id : "");
}

static void send_lab(const char *buf, size_t len)
{
    if (g.sock != PAL_SOCK_INVALID && g.lab_port != 0) {
        (void)pal_udp_sendto(g.sock, g.lab_ip, g.lab_port, buf, len);
    }
}

void ev_emit(const ev_t *e)
{
    static char out[4096];
    char clock[16], esc[EV_MAX_PAYLOAD * 2 + 8], hex[EV_MAX_RAW * 2 + 1];
    uint64_t now = pal_wall_ms();
    int n;

    /* ---- console ---- */
    pal_format_clock(now, clock);
    printf("[%s] %-5s %-6s -> %-6s %-18s", clock, e->proto, e->from ? e->from : "",
           e->to ? e->to : "", e->type);
    if (e->detail) {
        printf(" %s", e->detail);
    }
    if (e->len >= 0 || e->qos >= 0) {
        printf(" (");
        if (e->qos >= 0) {
            printf("qos %d%s", e->qos, e->len >= 0 ? ", " : "");
        }
        if (e->len >= 0) {
            printf("%d B", e->len);
        }
        printf(")");
    }
    if (e->lat_ms >= 0) {
        printf(" %.1f ms", e->lat_ms);
    }
    printf("\n");
    fflush(stdout);

    /* ---- lab ---- */
    if (g.sock == PAL_SOCK_INVALID) {
        return;
    }
    n = snprintf(out, sizeof(out),
                 "{\"k\":\"ev\",\"dev\":\"%s\",\"kind\":\"%s\",\"seq\":%u,\"t\":%llu,\"proto\":\"%s\","
                 "\"from\":\"%s\",\"to\":\"%s\",\"type\":\"%s\",\"sec\":\"%s\"",
                 g.dev, g.kind, ++g.seq, (unsigned long long)now, e->proto, e->from ? e->from : "",
                 e->to ? e->to : "", e->type, g.security);
    if (g.run[0]) {
        n += snprintf(out + n, sizeof(out) - (size_t)n, ",\"run\":\"%s\"", g.run);
    }
    if (e->qos >= 0) {
        n += snprintf(out + n, sizeof(out) - (size_t)n, ",\"qos\":%d", e->qos);
    }
    if (e->len >= 0) {
        n += snprintf(out + n, sizeof(out) - (size_t)n, ",\"len\":%d", e->len);
    }
    if (e->payload_len >= 0) {
        n += snprintf(out + n, sizeof(out) - (size_t)n, ",\"plen\":%d", e->payload_len);
    }
    if (e->lat_ms >= 0) {
        n += snprintf(out + n, sizeof(out) - (size_t)n, ",\"lat\":%.3f", e->lat_ms);
    }
    if (e->detail) {
        json_escape(esc, sizeof(esc), e->detail, 200);
        n += snprintf(out + n, sizeof(out) - (size_t)n, ",\"detail\":\"%s\"", esc);
    }
    if (e->payload) {
        json_escape(esc, sizeof(esc), e->payload, EV_MAX_PAYLOAD);
        n += snprintf(out + n, sizeof(out) - (size_t)n, ",\"payload\":\"%s\"", esc);
    }
    if (e->raw && e->raw_len) {
        size_t m = e->raw_len < EV_MAX_RAW ? e->raw_len : EV_MAX_RAW;
        for (size_t i = 0; i < m; i++) {
            snprintf(hex + 2 * i, 3, "%02x", e->raw[i]);
        }
        hex[2 * m] = '\0';
        n += snprintf(out + n, sizeof(out) - (size_t)n, ",\"raw\":\"%s\",\"rawlen\":%u", hex,
                      (unsigned)e->raw_len);
    }
    if (e->extra && e->extra[0]) {
        n += snprintf(out + n, sizeof(out) - (size_t)n, ",%s", e->extra);
    }
    if (n > 0 && (size_t)n < sizeof(out) - 2) {
        out[n++] = '}';
        out[n] = '\0';
        send_lab(out, (size_t)n);
    }
}

void ev_json(const char *k, const char *members_fmt, ...)
{
    static char out[EV_JSON_MAX];
    va_list ap;
    int n;

    if (g.sock == PAL_SOCK_INVALID) {
        return;
    }
    n = snprintf(out, sizeof(out), "{\"k\":\"%s\",\"dev\":\"%s\",\"kind\":\"%s\",\"t\":%llu", k,
                 g.dev, g.kind, (unsigned long long)pal_wall_ms());
    if (g.run[0]) {
        n += snprintf(out + n, sizeof(out) - (size_t)n, ",\"run\":\"%s\"", g.run);
    }
    if (members_fmt && members_fmt[0]) {
        out[n++] = ',';
        va_start(ap, members_fmt);
        n += vsnprintf(out + n, sizeof(out) - (size_t)n, members_fmt, ap);
        va_end(ap);
    }
    if (n > 0 && (size_t)n < sizeof(out) - 2) {
        out[n++] = '}';
        out[n] = '\0';
        send_lab(out, (size_t)n);
    }
}

void ev_log(const char *proto, const char *fmt, ...)
{
    char msg[512], esc[1100], clock[16];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    pal_format_clock(pal_wall_ms(), clock);
    printf("[%s] %-5s %s\n", clock, proto, msg);
    fflush(stdout);
    json_escape(esc, sizeof(esc), msg, sizeof(msg));
    ev_json("log", "\"proto\":\"%s\",\"msg\":\"%s\"", proto, esc);
}
