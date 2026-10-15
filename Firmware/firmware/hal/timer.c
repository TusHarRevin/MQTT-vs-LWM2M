#include "hal/timer.h"

#include "platform/port.h"

uint32_t hal_millis(void)
{
    return port_millis();
}

uint32_t hal_uptime_s(void)
{
    static uint32_t last_ms;
    static uint64_t total_ms;
    uint32_t now = port_millis();

    total_ms += (uint32_t)(now - last_ms);
    last_ms = now;
    return (uint32_t)(total_ms / 1000u);
}

void hal_delay_ms(uint32_t ms)
{
    port_delay_ms(ms);
}

void hal_timer_start(hal_timer_t *t, uint32_t period_ms)
{
    t->start = port_millis();
    t->period = period_ms;
    t->running = true;
}

void hal_timer_stop(hal_timer_t *t)
{
    t->running = false;
}

bool hal_timer_expired(const hal_timer_t *t)
{
    return t->running && (uint32_t)(port_millis() - t->start) >= t->period;
}

uint32_t hal_timer_remaining(const hal_timer_t *t)
{
    uint32_t elapsed;

    if (!t->running) {
        return FW_WAIT_FOREVER;
    }
    elapsed = (uint32_t)(port_millis() - t->start);
    return elapsed >= t->period ? 0 : t->period - elapsed;
}

void hal_timer_restart(hal_timer_t *t)
{
    uint32_t now = port_millis();

    t->start += t->period;
    if ((uint32_t)(now - t->start) >= t->period) {
        t->start = now; /* fell more than a period behind: resync */
    }
    t->running = true;
}
