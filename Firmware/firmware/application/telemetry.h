#ifndef APP_TELEMETRY_H
#define APP_TELEMETRY_H

#include "common/fw_types.h"

int telemetry_init(uint32_t interval_ms);
void telemetry_set_interval(uint32_t interval_ms);
uint32_t telemetry_interval(void);
/* Samples sensors and publishes when the interval has elapsed. */
void telemetry_step(void);

#endif /* APP_TELEMETRY_H */
