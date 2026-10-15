#ifndef HAL_TIMER_H
#define HAL_TIMER_H

#include "common/fw_types.h"

/* Software deadline timer, polled from the main loop. Wrap-safe. */
typedef struct {
    uint32_t start;
    uint32_t period;
    bool running;
} hal_timer_t;

uint32_t hal_millis(void);
uint32_t hal_uptime_s(void); /* does not wrap at 49 days; call at least once per 49 days */
void hal_delay_ms(uint32_t ms);

void hal_timer_start(hal_timer_t *t, uint32_t period_ms);
void hal_timer_stop(hal_timer_t *t);
bool hal_timer_expired(const hal_timer_t *t);
/* ms until expiry: 0 if expired, FW_WAIT_FOREVER if stopped. */
uint32_t hal_timer_remaining(const hal_timer_t *t);
/* Periodic re-arm: advances by one period to avoid drift, resyncs if late. */
void hal_timer_restart(hal_timer_t *t);

#endif /* HAL_TIMER_H */
