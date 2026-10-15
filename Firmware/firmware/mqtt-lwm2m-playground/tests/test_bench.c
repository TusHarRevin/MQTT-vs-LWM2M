/* Percentiles (nearest rank) and summaries over known data. */
#include <string.h>

#include "bench.h"
#include "check.h"

int main(void)
{
    double v[100];
    bench_t b;
    bench_summary_t s;

    for (int i = 0; i < 100; i++) {
        v[i] = i + 1; /* 1..100 */
    }
    CHECK_NEAR(bench_percentile(v, 100, 95), 95, 1e-9);
    CHECK_NEAR(bench_percentile(v, 100, 99), 99, 1e-9);
    CHECK_NEAR(bench_percentile(v, 100, 100), 100, 1e-9);
    CHECK_NEAR(bench_percentile(v, 1, 95), 1, 1e-9);
    CHECK(bench_percentile(v, 0, 95) < 0);

    bench_init(&b, "TEST", "t1");
    for (int i = 1; i <= 10; i++) {
        bench_rec_t r;
        memset(&r, 0, sizeof(r));
        r.seq = (uint32_t)i;
        r.ok = i != 10; /* last one fails */
        r.lat_ms = r.ok ? i : -1;
        r.tx_bytes = 100;
        r.rx_bytes = 4;
        r.payload = 80;
        bench_add(&b, &r);
    }
    bench_summarize(&b, &s);
    CHECK(s.n == 10 && s.ok == 9 && s.fail == 1);
    CHECK_NEAR(s.success_pct, 90.0, 1e-9);
    CHECK_NEAR(s.min, 1, 1e-9);
    CHECK_NEAR(s.max, 9, 1e-9);
    CHECK_NEAR(s.avg, 5, 1e-9);
    CHECK_NEAR(s.median, 5, 1e-9);
    CHECK_NEAR(s.p95, 9, 1e-9);
    CHECK_NEAR(s.avg_total, 104, 1e-9);
    bench_free(&b);

    /* no successes: latency stays "not measured" (null), never 0 */
    bench_init(&b, "TEST", "t2");
    {
        bench_rec_t r;
        char json[2048];
        memset(&r, 0, sizeof(r));
        r.lat_ms = -1;
        bench_add(&b, &r);
        bench_summarize(&b, &s);
        CHECK(s.ok == 0 && s.min < 0 && s.p99 < 0);
        bench_summary_json(&b, &s, json, sizeof(json));
        CHECK(strstr(json, "\"lat_avg\":null") != NULL);
    }
    bench_free(&b);
    CHECK_DONE();
}
