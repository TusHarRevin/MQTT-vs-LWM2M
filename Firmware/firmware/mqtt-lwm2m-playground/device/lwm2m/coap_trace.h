#ifndef COAP_TRACE_H
#define COAP_TRACE_H

/*
 * CoAP message decoder + LwM2M naming, for the event log and latency.
 *
 * Wakaama builds and parses the real messages; this module only *looks* at
 * the bytes going in and out so each one can be labelled the way the LwM2M
 * spec talks about it (REGISTER, READ, OBSERVE, NOTIFY, ...) and so a
 * confirmable message can be matched to its ACK by Message ID.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define COAP_CON 0
#define COAP_NON 1
#define COAP_ACK 2
#define COAP_RST 3

typedef struct
{
    uint8_t type, tkl, code;
    uint16_t mid;
    uint8_t token[8];
    char uri[96];     /* "/rd?ep=x&lt=300" or "/3303/0/5700" */
    int path_depth;   /* number of Uri-Path segments */
    char first_seg[8];
    bool has_observe;
    uint32_t observe;
    int content_format; /* -1 = none */
    int accept;         /* -1 = none */
    size_t payload_off, payload_len;
} coap_info_t;

/* Returns 0 on success, -1 if the bytes are not a valid CoAP message. */
int coap_parse(const uint8_t *buf, size_t len, coap_info_t *ci);
const char *coap_type_name(uint8_t type);
void coap_code_str(uint8_t code, char out[8]); /* "2.05", "0.01" */
const char *coap_method_name(uint8_t code);    /* GET/POST/PUT/DELETE or NULL */
const char *coap_format_name(int cf);

/* Remembers outstanding requests so responses can be named and timed. */
typedef struct
{
    bool used;
    uint16_t mid;
    char name[28];
    uint64_t t_us;
    uint8_t sends;
} coap_pending_t;

typedef struct
{
    coap_pending_t tx[32], rx[32];
} coap_tracker_t;

/*
 * Names one message and, for a response/ACK, computes the latency from its
 * request. from_device: true for messages the device sends.
 * Returns the number of times this request MID has been sent (> 1 means a
 * CoAP retransmission), 0 for responses.
 */
int coap_track(coap_tracker_t *t, const coap_info_t *ci, bool from_device, uint64_t now_us,
               char *name, size_t name_cap, double *lat_ms);

#endif /* COAP_TRACE_H */
