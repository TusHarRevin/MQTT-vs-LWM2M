#include "application/sensor.h"

#include "common/fw_log.h"
#include "platform/port.h"

#define TAG "sensor"

/* Exponential moving average weight of each new sample. */
#define SENSOR_EMA_ALPHA 0.3f

static const sensor_info_t s_info[SENSOR_COUNT] = {
    [SENSOR_TEMPERATURE] = { "temperature", "Cel", "/3303/0/5700", 0 },
    [SENSOR_HUMIDITY]    = { "humidity",    "%RH", "/3304/0/5700", 1 },
};

static struct {
    float value;
    bool valid;
} s_state[SENSOR_COUNT];

int sensor_init(void)
{
    for (int i = 0; i < SENSOR_COUNT; i++) {
        s_state[i].valid = false;
    }
    return FW_OK;
}

int sensor_sample_all(void)
{
    int failures = 0;

    for (int i = 0; i < SENSOR_COUNT; i++) {
        float raw;
        int rc = port_sensor_read(s_info[i].channel, &raw);

        if (rc != FW_OK) {
            LOG_D(TAG, "%s read failed (%d)", s_info[i].name, rc);
            failures++;
            continue;
        }
        if (!s_state[i].valid) {
            s_state[i].value = raw;
            s_state[i].valid = true;
        } else {
            s_state[i].value += SENSOR_EMA_ALPHA * (raw - s_state[i].value);
        }
    }
    return failures;
}

bool sensor_get(sensor_id_t id, float *value)
{
    if ((unsigned)id >= SENSOR_COUNT || !s_state[id].valid) {
        return false;
    }
    *value = s_state[id].value;
    return true;
}

const sensor_info_t *sensor_info(sensor_id_t id)
{
    return (unsigned)id < SENSOR_COUNT ? &s_info[id] : NULL;
}
