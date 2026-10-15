#ifndef HAL_POWER_H
#define HAL_POWER_H

#include "common/fw_types.h"

int hal_power_battery_mv(void); /* < 0 if unknown */
bool hal_power_battery_low(void);
/* Sleep in the lowest power mode that still wakes within max_ms. */
void hal_power_idle(uint32_t max_ms);
void hal_power_reset(void);

#endif /* HAL_POWER_H */
