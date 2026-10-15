#include "hal/power.h"

#include "common/fw_config.h"
#include "common/fw_log.h"
#include "platform/port.h"

#define TAG "power"

int hal_power_battery_mv(void)
{
    return port_battery_mv();
}

bool hal_power_battery_low(void)
{
    int mv = port_battery_mv();

    return mv >= 0 && mv < FW_BATTERY_LOW_MV;
}

void hal_power_idle(uint32_t max_ms)
{
    if (max_ms == 0) {
        return;
    }
    /* TODO: pick a deeper sleep mode (STOP/STANDBY) for long idle periods
     * once RTC wake-up is wired in the platform port. */
    port_sleep_ms(max_ms);
}

void hal_power_reset(void)
{
    LOG_I(TAG, "system reset");
    port_reset();
}
