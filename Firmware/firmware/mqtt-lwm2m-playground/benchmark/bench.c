#include "bench.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pal.h"

void bench_init(bench_t *b, const char *proto, const char *run_id)
{
    memset(b, 0, sizeof(*b));
    snprintf(b->proto, sizeof(b->proto), "%s", proto);
    snprintf(b->run_id, sizeof(b->run_id), "%s", run_id);
    snprintf(b->security, sizeof(b->security), "none");
    b->started_ms = pal_wall_ms();
    b->connect_ms = b->handshake_ms = b->session_ms = -1.0;
}

void bench_free(bench_t *b)
{
    free(b->recs);
    b->recs = NULL;
    b->n = b->cap = 0;
}

void bench_add(bench_t *b, const bench_rec_t *r)
{
    if (b->n == b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 128;
        bench_rec_t *p = realloc(b->recs, nc * sizeof(*p));
        if (p == NULL) {
            return;
        }
        b->recs = p;
        b->cap = nc;
    }
    b->recs[b->n++] = *r;
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

double bench_percentile(const double *sorted, size_t n, double p)
{
    size_t rank;
    if (n == 0) {
        return -1.0;
    }
    /* nearest rank: ceil(p/100 * n), 1-based */
    rank = (size_t)ceil(p / 100.0 * (double)n);
    if (rank < 1) {
        rank = 1;
    }
    if (rank > n) {
        rank = n;
    }
    return sorted[rank - 1];
}

void bench_summarize(const bench_t *b, bench_summary_t *s)
{
    double *lat = NULL;
    double sum = 0, tx = 0, rx = 0, pl = 0;

    memset(s, 0, sizeof(*s));
    s->min = s->avg = s->median = s->p95 = s->p99 = s->max = -1.0;
    s->n = b->n;
    if (b->n == 0) {
        return;
    }
    lat = malloc(b->n * sizeof(double));
    for (size_t i = 0; i < b->n; i++) {
        const bench_rec_t *r = &b->recs[i];
        tx += r->tx_bytes;
        rx += r->rx_bytes;
        pl += r->payload;
        if (r->ok && r->lat_ms >= 0) {
            lat[s->ok++] = r->lat_ms;
            sum += r->lat_ms;
        }
    }
    s->fail = s->n - s->ok;
    s->success_pct = 100.0 * (double)s->ok / (double)s->n;
    s->avg_tx = tx / (double)s->n;
    s->avg_rx = rx / (double)s->n;
    s->avg_total = (tx + rx) / (double)s->n;
    s->avg_payload = pl / (double)s->n;
    if (s->ok > 0 && lat != NULL) {
        qsort(lat, s->ok, sizeof(double), cmp_double);
        s->min = lat[0];
        s->max = lat[s->ok - 1];
        s->avg = sum / (double)s->ok;
        s->median = s->ok % 2 ? lat[s->ok / 2] : (lat[s->ok / 2 - 1] + lat[s->ok / 2]) / 2.0;
        s->p95 = bench_percentile(lat, s->ok, 95.0);
        s->p99 = bench_percentile(lat, s->ok, 99.0);
    }
    free(lat);
}

int bench_write_csv(const bench_t *b, const char *dir, char *path_out, size_t path_cap)
{
    char path[512];
    FILE *f;

    pal_mkdir(dir); /* fine if it already exists */
    snprintf(path, sizeof(path), "%s/%s.csv", dir, b->run_id);
    f = fopen(path, "w");
    if (f == NULL) {
        return -1;
    }
    fprintf(f, "seq,timestamp_ms,latency_ms,payload_bytes,bytes_sent,bytes_received,success,retries\n");
    for (size_t i = 0; i < b->n; i++) {
        const bench_rec_t *r = &b->recs[i];
        if (r->ok) {
            fprintf(f, "%u,%llu,%.3f,%u,%u,%u,1,%u\n", r->seq, (unsigned long long)r->t_ms,
                    r->lat_ms, r->payload, r->tx_bytes, r->rx_bytes, r->retries);
        } else {
            fprintf(f, "%u,%llu,,%u,%u,%u,0,%u\n", r->seq, (unsigned long long)r->t_ms, r->payload,
                    r->tx_bytes, r->rx_bytes, r->retries);
        }
    }
    fclose(f);
    if (path_out) {
        snprintf(path_out, path_cap, "%s", path);
    }
    return 0;
}

/* JSON number or null for "not measured". */
static int jnum(char *buf, size_t cap, const char *name, double v)
{
    return v < 0 ? snprintf(buf, cap, ",\"%s\":null", name)
                 : snprintf(buf, cap, ",\"%s\":%.3f", name, v);
}

int bench_summary_json(const bench_t *b, const bench_summary_t *s, char *buf, size_t cap)
{
    int n = snprintf(buf, cap,
                     "{\"run\":\"%s\",\"proto\":\"%s\",\"started\":%llu,\"ended\":%llu,"
                     "\"security\":\"%s\",\"n\":%u,\"ok\":%u,\"fail\":%u,\"success_pct\":%.2f",
                     b->run_id, b->proto, (unsigned long long)b->started_ms,
                     (unsigned long long)pal_wall_ms(), b->security, (unsigned)s->n,
                     (unsigned)s->ok, (unsigned)s->fail, s->success_pct);
    n += jnum(buf + n, cap - (size_t)n, "lat_min", s->min);
    n += jnum(buf + n, cap - (size_t)n, "lat_avg", s->avg);
    n += jnum(buf + n, cap - (size_t)n, "lat_median", s->median);
    n += jnum(buf + n, cap - (size_t)n, "lat_p95", s->p95);
    n += jnum(buf + n, cap - (size_t)n, "lat_p99", s->p99);
    n += jnum(buf + n, cap - (size_t)n, "lat_max", s->max);
    n += jnum(buf + n, cap - (size_t)n, "connect_ms", b->connect_ms);
    n += jnum(buf + n, cap - (size_t)n, "handshake_ms", b->handshake_ms);
    n += jnum(buf + n, cap - (size_t)n, "session_ms", b->session_ms);
    n += snprintf(buf + n, cap - (size_t)n,
                  ",\"hs_bytes\":%llu,\"avg_tx\":%.1f,\"avg_rx\":%.1f,\"bytes_per_msg\":%.1f,"
                  "\"avg_payload\":%.1f",
                  (unsigned long long)b->hs_bytes, s->avg_tx, s->avg_rx, s->avg_total,
                  s->avg_payload);
    if (b->config[0]) {
        n += snprintf(buf + n, cap - (size_t)n, ",%s", b->config);
    }
    n += snprintf(buf + n, cap - (size_t)n, "}");
    return n;
}
