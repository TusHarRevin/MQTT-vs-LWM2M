#ifndef SENSOR_H
#define SENSOR_H

/*
 * Simulated environmental sensor.
 *
 * Each channel is a mean-reverting random walk (discrete Ornstein-Uhlenbeck
 * process) clamped to its physical range, so values drift gradually instead
 * of jumping. The same seed always produces the same sequence.
 *
 * On hardware, replace sensor_step() with real driver reads; nothing else
 * depends on how the numbers are produced.
 */

#include <stdint.h>

#include "pal.h"

typedef struct
{
    float temperature; /* °C,  20 .. 35   */
    float humidity;    /* %RH, 30 .. 90   */
    float pressure;    /* hPa, 990 .. 1030 */
    uint16_t battery;  /* %,   0 .. 100   */
} SensorData_t;

#define SENSOR_TEMP_MIN 20.0f
#define SENSOR_TEMP_MAX 35.0f
#define SENSOR_HUM_MIN 30.0f
#define SENSOR_HUM_MAX 90.0f
#define SENSOR_PRES_MIN 990.0f
#define SENSOR_PRES_MAX 1030.0f

typedef struct
{
    pal_rng_t rng;
    double temperature, humidity, pressure, battery;
    uint32_t samples;
} sensor_sim_t;

void sensor_init(sensor_sim_t *s, uint64_t seed);
/* Advance one sample period and return the new reading. */
void sensor_step(sensor_sim_t *s, SensorData_t *out);
/* Current reading without advancing. */
void sensor_peek(const sensor_sim_t *s, SensorData_t *out);

#endif /* SENSOR_H */
