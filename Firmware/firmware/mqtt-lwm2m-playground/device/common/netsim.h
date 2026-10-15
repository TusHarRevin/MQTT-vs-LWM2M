#ifndef NETSIM_H
#define NETSIM_H

/*
 * Application-level network impairment ("netem in a library").
 *
 * Used when Linux `tc netem` is not available (Windows, macOS, CI). It sits
 * between the protocol stacks and the real socket, inside link.c, so TLS and
 * DTLS records are impaired too.
 *
 * Model: a delay line in each direction.
 *
 *   latency  Every packet is held for `latency_ms` before it goes out, and
 *            every arriving packet is held for `latency_ms` before the
 *            device sees it. A request/response therefore takes 2 x latency
 *            plus the real round trip, no matter how many packets are in
 *            flight; a server-initiated request arrives 1 x latency late.
 *
 *   loss     UDP: the datagram is really dropped (either direction). CoAP and
 *            DTLS must retransmit, exactly as on a lossy radio link.
 *            TCP: dropping bytes would corrupt the stream, so a "lost"
 *            segment is delivered one retransmission timeout later instead
 *            (rto_ms, doubling for consecutive losses) - which is what the
 *            TCP stack would do. Order is preserved.
 *
 * This is a teaching model, not a packet-accurate emulator: no jitter, no
 * bandwidth limit, no reordering. For that use `tc netem` on Linux/WSL (see
 * scripts/netem.sh and docs/benchmarking.md).
 */

#include <stdbool.h>
#include <stdint.h>

#include "pal.h"

typedef struct
{
    uint32_t latency_ms; /* one-way */
    double loss;         /* 0.0 .. 1.0 */
    uint32_t rto_ms;     /* TCP retransmission timeout model (default 200) */
    uint64_t seed;
} netsim_cfg_t;

typedef struct
{
    netsim_cfg_t cfg;
    pal_rng_t rng;
    /* Counters, for the dashboard. */
    uint32_t dropped_tx, dropped_rx, tcp_retx;
    double injected_ms;
} netsim_t;

typedef struct
{
    uint32_t delay_ms; /* hold the packet this long */
    bool drop;         /* UDP only: discard it */
} netsim_verdict_t;

void netsim_init(netsim_t *n, const netsim_cfg_t *cfg);
bool netsim_active(const netsim_t *n);
/* Verdict for one packet leaving / arriving at the device. */
netsim_verdict_t netsim_on_send(netsim_t *n, bool udp);
netsim_verdict_t netsim_on_receive(netsim_t *n, bool udp);

/* Named profiles. Returns false for an unknown name. "custom" keeps cfg. */
bool netsim_profile(const char *name, netsim_cfg_t *cfg);

#endif /* NETSIM_H */
