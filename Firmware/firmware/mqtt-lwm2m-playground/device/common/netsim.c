#include "netsim.h"

#include <string.h>

void netsim_init(netsim_t *n, const netsim_cfg_t *cfg)
{
    memset(n, 0, sizeof(*n));
    n->cfg = *cfg;
    if (n->cfg.rto_ms == 0) {
        n->cfg.rto_ms = 200; /* Linux TCP_RTO_MIN */
    }
    if (n->cfg.loss < 0.0) {
        n->cfg.loss = 0.0;
    }
    if (n->cfg.loss > 1.0) {
        n->cfg.loss = 1.0;
    }
    pal_rng_seed(&n->rng, cfg->seed ^ 0xA5A5A5A5ULL);
}

bool netsim_active(const netsim_t *n)
{
    return n != NULL && (n->cfg.latency_ms > 0 || n->cfg.loss > 0.0);
}

static netsim_verdict_t decide(netsim_t *n, bool udp, int dir)
{
    netsim_verdict_t v = { 0, false };

    if (!netsim_active(n)) {
        return v;
    }
    v.delay_ms = n->cfg.latency_ms; /* every packet, both directions */
    if (n->cfg.loss > 0.0) {
        if (udp) {
            if (pal_rng_unit(&n->rng) < n->cfg.loss) {
                v.drop = true;
                if (dir == 1) {
                    n->dropped_tx++;
                } else {
                    n->dropped_rx++;
                }
            }
        } else {
            uint32_t rto = n->cfg.rto_ms;
            int tries = 0;
            /* Each loss costs an RTO; consecutive losses back off. */
            while (tries < 6 && pal_rng_unit(&n->rng) < n->cfg.loss) {
                v.delay_ms += rto;
                rto *= 2;
                tries++;
                n->tcp_retx++;
            }
        }
    }
    if (!v.drop) {
        n->injected_ms += v.delay_ms;
    }
    return v;
}

netsim_verdict_t netsim_on_send(netsim_t *n, bool udp)
{
    return decide(n, udp, 1);
}

netsim_verdict_t netsim_on_receive(netsim_t *n, bool udp)
{
    return decide(n, udp, 2);
}

bool netsim_profile(const char *name, netsim_cfg_t *cfg)
{
    static const struct {
        const char *name;
        uint32_t latency_ms;
        double loss;
    } profiles[] = {
        { "excellent", 0, 0.0 },
        { "normal", 0, 0.0 },
        { "good", 50, 0.01 },
        { "poor", 250, 0.05 },
        { "very-poor", 1000, 0.10 },
        { "very_poor", 1000, 0.10 },
    };
    if (strcmp(name, "custom") == 0) {
        return true;
    }
    for (size_t i = 0; i < sizeof(profiles) / sizeof(profiles[0]); i++) {
        if (strcmp(name, profiles[i].name) == 0) {
            cfg->latency_ms = profiles[i].latency_ms;
            cfg->loss = profiles[i].loss;
            return true;
        }
    }
    return false;
}
