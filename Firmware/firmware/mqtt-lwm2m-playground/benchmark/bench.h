#ifndef BENCH_H
#define BENCH_H

/*
 * Benchmark recorder: one record per message, a summary at the end.
 *
 * Latency definitions (see docs/benchmarking.md):
 *   MQTT QoS 0  PUBLISH -> broker receipt, proven by a trailing PINGREQ whose
 *               PINGRESP can only come back after the broker has read the
 *               PUBLISH (same TCP stream, processed in order)
 *   MQTT QoS 1  PUBLISH -> PUBACK
 *   MQTT QoS 2  PUBLISH -> PUBCOMP (PUBREC and PUBREL in between)
 *   LwM2M       CON -> ACK (notifications are sent confirmable)
 *
 * Percentiles use the nearest-rank method over successful messages only.
 * Nothing here is estimated: a failed message has no latency.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct
{
    uint32_t seq;
    uint64_t t_ms;      /* wall clock when sent */
    double lat_ms;      /* < 0 if failed */
    uint32_t payload;   /* application payload bytes */
    uint32_t tx_bytes;  /* wire bytes sent for this message (incl. acks we sent) */
    uint32_t rx_bytes;  /* wire bytes received for it */
    uint8_t ok;
    uint8_t retries;    /* CoAP retransmissions */
} bench_rec_t;

typedef struct
{
    size_t n, ok, fail;
    double min, avg, median, p95, p99, max; /* ms, successful only */
    double success_pct;
    double avg_tx, avg_rx, avg_total;       /* wire bytes per message */
    double avg_payload;
} bench_summary_t;

typedef struct
{
    char run_id[48];
    char proto[8];
    char config[768];   /* JSON members describing the setup */
    bench_rec_t *recs;
    size_t n, cap;
    uint64_t started_ms;
    /* connection setup, measured once per run */
    double connect_ms, handshake_ms, session_ms;
    uint64_t hs_bytes;
    char security[32];
} bench_t;

void bench_init(bench_t *b, const char *proto, const char *run_id);
void bench_free(bench_t *b);
void bench_add(bench_t *b, const bench_rec_t *r);
void bench_summarize(const bench_t *b, bench_summary_t *s);
/* Nearest-rank percentile of an ascending array (p in 0..100). */
double bench_percentile(const double *sorted, size_t n, double p);
int bench_write_csv(const bench_t *b, const char *dir, char *path_out, size_t path_cap);
/* JSON object describing the run (config + summary). */
int bench_summary_json(const bench_t *b, const bench_summary_t *s, char *buf, size_t cap);

#endif /* BENCH_H */
