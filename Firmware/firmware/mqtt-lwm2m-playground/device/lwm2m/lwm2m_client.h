#ifndef LWM2M_CLIENT_H
#define LWM2M_CLIENT_H

/*
 * Wakaama glue: owns the UDP (or DTLS) link, feeds Wakaama, traces every
 * CoAP message and makes notifications confirmable.
 *
 * Why confirmable notifications? Wakaama sends Observe notifications as NON
 * (no acknowledgement), so their delivery and latency can't be measured. RFC
 * 7641 allows CON notifications; we flip the type of each outgoing
 * notification to CON and handle the ACK / retransmission (RFC 7252 timing:
 * ACK_TIMEOUT 2 s x [1, 1.5], doubled per retry, MAX_RETRANSMIT 4) here.
 * Use --notify-non to see the stock behaviour.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "link.h"
#include "netsim.h"

typedef struct
{
    char path[48];
    uint16_t mid;
    uint64_t t_ms;       /* wall clock of first transmission */
    double lat_ms;       /* CON -> ACK, < 0 if failed or NON */
    bool ok;
    int retries;
    uint32_t tx_bytes;   /* wire bytes incl. retransmissions */
    uint32_t rx_bytes;
    uint32_t payload;    /* CoAP payload bytes */
} lwc_notify_t;

typedef struct
{
    const char *endpoint;
    const char *host;
    uint16_t port;
    bool secure;
    const char *psk_identity;
    const uint8_t *psk;
    size_t psk_len;
    uint32_t lifetime_s;
    bool notify_con;
    uint32_t ack_timeout_ms;
    uint32_t dtls_timeout_ms;
    netsim_t *sim;
    void (*on_notify)(const lwc_notify_t *n); /* called once per notification outcome */
} lwc_cfg_t;

typedef struct
{
    double connect_ms, handshake_ms, register_ms; /* register_ms = start -> registered */
    uint64_t hs_bytes;
    char security[32];
    bool have_session;
} lwc_session_t;

int lwc_start(const lwc_cfg_t *cfg);
/* Runs Wakaama, retransmissions and receives for up to max_wait_ms. */
int lwc_step(uint32_t max_wait_ms);
bool lwc_registered(void);
bool lwc_running(void);
void lwc_value_changed(const char *path);
void lwc_trigger_update(void);
/* Sends De-register and waits briefly for its ACK, then stops. */
void lwc_deregister(void);
void lwc_stop(void);
const lwc_session_t *lwc_session(void);
const link_t *lwc_link(void);
/* Comma-separated list of observed paths, e.g. "/3303/0/5700,/3/0/9". */
void lwc_observed(char *out, size_t cap);
uint32_t lwc_pending_notifies(void);

#endif /* LWM2M_CLIENT_H */
