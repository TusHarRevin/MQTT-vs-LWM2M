#ifndef EVENTS_H
#define EVENTS_H

/*
 * Protocol event reporting.
 *
 * Every protocol message the device sends or receives is reported twice:
 *   1. a human-readable line on stdout:
 *        [12:31:01.130] MQTT  DEVICE -> BROKER  PUBLISH  temperature=27.40 (qos 1, 142 B)
 *   2. a JSON datagram to the lab server (UDP 127.0.0.1:7000 by default),
 *      which forwards it to the dashboard. If the lab isn't running the
 *      datagrams are simply lost; the device doesn't care.
 *
 * On hardware, events can go to a debug UART instead.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct
{
    const char *proto;   /* "MQTT", "LWM2M", "OTA", "NET", "TLS", "DTLS" */
    const char *from;    /* "DEVICE", "BROKER", "SERVER" */
    const char *to;
    const char *type;    /* "CONNECT", "PUBLISH", "NOTIFY", ... */
    int qos;             /* -1 = n/a */
    int len;             /* protocol message length, -1 = n/a */
    int payload_len;     /* application payload length, -1 = n/a */
    double lat_ms;       /* < 0 = n/a */
    const char *detail;  /* short human text */
    const char *payload; /* application payload as text (truncated) */
    const uint8_t *raw;  /* raw protocol bytes (hex-dumped, truncated) */
    size_t raw_len;
    const char *extra;   /* extra JSON members, e.g. "\"mid\":17" (no braces) */
} ev_t;

#define EV_INIT { NULL, "DEVICE", NULL, NULL, -1, -1, -1, -1.0, NULL, NULL, NULL, 0, NULL }

void ev_init(const char *device_id, const char *kind, const char *lab_ip, uint16_t lab_port);
void ev_set_security(const char *label);
void ev_set_run(const char *run_id);
void ev_emit(const ev_t *e);
/* Free-form JSON object sent to the lab (no console line), e.g. status. The
 * caller passes the members; "k", "dev" and "t" are added. */
void ev_json(const char *k, const char *members_fmt, ...);
/* Console + lab "LOG" event. */
void ev_log(const char *proto, const char *fmt, ...);

/* JSON string escaping; always NUL-terminates. Returns bytes written. */
size_t json_escape(char *dst, size_t cap, const char *src, size_t max_src);

#endif /* EVENTS_H */
