/* The impairment model: latency on every packet in both directions, UDP
 * loss rate, TCP loss -> retransmission penalty, never a drop. */
#include "check.h"
#include "netsim.h"

int main(void)
{
    netsim_t n;
    netsim_cfg_t cfg = { 100, 0.0, 200, 7 };
    netsim_verdict_t v;
    int drops = 0;

    netsim_init(&n, &cfg);
    v = netsim_on_send(&n, true);
    CHECK(v.delay_ms == 100 && !v.drop);
    v = netsim_on_send(&n, true); /* back-to-back packets are each delayed */
    CHECK(v.delay_ms == 100);
    v = netsim_on_receive(&n, true);
    CHECK(v.delay_ms == 100);

    cfg.latency_ms = 0;
    cfg.loss = 0.10;
    netsim_init(&n, &cfg);
    for (int i = 0; i < 20000; i++) {
        drops += netsim_on_send(&n, true).drop;
    }
    CHECK(drops > 1700 && drops < 2300); /* ~10 % */

    netsim_init(&n, &cfg);
    for (int i = 0; i < 2000; i++) {
        v = netsim_on_send(&n, false);
        CHECK(!v.drop);               /* TCP never drops */
        CHECK(v.delay_ms % 200 == 0); /* latency 0 here: only RTO multiples */
    }
    CHECK(n.tcp_retx > 100);

    {
        netsim_cfg_t p = { 0 };
        CHECK(netsim_profile("poor", &p) && p.latency_ms == 250 && p.loss > 0.049 && p.loss < 0.051);
        CHECK(!netsim_profile("nonsense", &p));
    }
    cfg.loss = 0;
    cfg.latency_ms = 0;
    netsim_init(&n, &cfg);
    CHECK(!netsim_active(&n));
    CHECK_DONE();
}
