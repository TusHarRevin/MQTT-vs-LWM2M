#include "sensor.h"

static double clampd(double v, double lo, double hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* One OU step: pull towards `mean` by `theta`, plus gaussian noise. */
static double ou_step(pal_rng_t *r, double x, double mean, double theta, double sigma,
                      double lo, double hi)
{
    return clampd(x + theta * (mean - x) + sigma * pal_rng_gauss(r), lo, hi);
}

void sensor_init(sensor_sim_t *s, uint64_t seed)
{
    pal_rng_seed(&s->rng, seed);
    /* Start somewhere plausible, but seed-dependent. */
    s->temperature = 24.0 + 6.0 * pal_rng_unit(&s->rng);
    s->humidity = 45.0 + 25.0 * pal_rng_unit(&s->rng);
    s->pressure = 1000.0 + 20.0 * pal_rng_unit(&s->rng);
    s->battery = 80.0 + 20.0 * pal_rng_unit(&s->rng);
    s->samples = 0;
}

void sensor_peek(const sensor_sim_t *s, SensorData_t *out)
{
    out->temperature = (float)s->temperature;
    out->humidity = (float)s->humidity;
    out->pressure = (float)s->pressure;
    out->battery = (uint16_t)(s->battery + 0.5);
}

void sensor_step(sensor_sim_t *s, SensorData_t *out)
{
    double t_prev = s->temperature;

    s->temperature = ou_step(&s->rng, s->temperature, 27.5, 0.02, 0.25,
                             SENSOR_TEMP_MIN, SENSOR_TEMP_MAX);
    /* Warmer air holds more water, so relative humidity dips as it warms. */
    s->humidity = ou_step(&s->rng, s->humidity - 1.5 * (s->temperature - t_prev), 60.0, 0.02, 0.6,
                          SENSOR_HUM_MIN, SENSOR_HUM_MAX);
    s->pressure = ou_step(&s->rng, s->pressure, 1012.0, 0.01, 0.25,
                          SENSOR_PRES_MIN, SENSOR_PRES_MAX);

    /* Slow, slightly noisy drain. At 0 % the simulated battery is swapped. */
    s->battery -= 0.05 + 0.03 * pal_rng_unit(&s->rng);
    if (s->battery < 0.0) {
        s->battery = 100.0;
    }
    s->samples++;
    sensor_peek(s, out);
}
