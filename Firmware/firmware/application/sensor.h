#ifndef APP_SENSOR_H
#define APP_SENSOR_H

#include "common/fw_types.h"

typedef enum {
    SENSOR_TEMPERATURE,
    SENSOR_HUMIDITY,
    SENSOR_COUNT
} sensor_id_t;

typedef struct {
    const char *name;      /* JSON key */
    const char *unit;
    const char *lwm2m_uri; /* IPSO resource, e.g. "/3303/0/5700" */
    unsigned channel;      /* port_sensor_read() channel */
} sensor_info_t;

int sensor_init(void);
/* Reads every sensor and updates its filtered value. Returns the number of
 * sensors that failed. */
int sensor_sample_all(void);
/* false if the sensor has never produced a valid reading. */
bool sensor_get(sensor_id_t id, float *value);
const sensor_info_t *sensor_info(sensor_id_t id);

#endif /* APP_SENSOR_H */
