/* The simulated sensor must stay in range, drift gradually and be
 * reproducible for a given seed. */
#include <math.h>

#include "check.h"
#include "sensor.h"

int main(void)
{
    sensor_sim_t a, b, c;
    SensorData_t da, db, dc, prev;
    int differs = 0;

    sensor_init(&a, 42);
    sensor_init(&b, 42);
    sensor_init(&c, 43);
    sensor_peek(&a, &prev);
    for (int i = 0; i < 20000; i++) {
        sensor_step(&a, &da);
        sensor_step(&b, &db);
        sensor_step(&c, &dc);
        CHECK(da.temperature >= 20.0f && da.temperature <= 35.0f);
        CHECK(da.humidity >= 30.0f && da.humidity <= 90.0f);
        CHECK(da.pressure >= 990.0f && da.pressure <= 1030.0f);
        CHECK(da.battery <= 100);
        /* same seed, same sequence */
        CHECK(da.temperature == db.temperature && da.humidity == db.humidity &&
              da.pressure == db.pressure && da.battery == db.battery);
        differs += da.temperature != dc.temperature;
        /* gradual: no big jumps between consecutive samples
         * (battery may wrap 0 -> 100 when the simulated battery is swapped) */
        CHECK(fabsf(da.temperature - prev.temperature) < 2.0f);
        CHECK(fabsf(da.humidity - prev.humidity) < 6.0f);
        CHECK(fabsf(da.pressure - prev.pressure) < 2.0f);
        if (check_failures) {
            break;
        }
        prev = da;
    }
    CHECK(differs > 1000); /* a different seed gives a different sequence */
    CHECK_DONE();
}
