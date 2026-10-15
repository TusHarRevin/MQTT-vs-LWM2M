#ifndef TELEMETRY_H
#define TELEMETRY_H

#include <stddef.h>
#include <stdint.h>

#include "sensor.h"

/*
 * Builds the JSON telemetry document both protocols carry:
 *
 *   {"dev":"mqtt-1","seq":12,"ts":1730000000000,"temperature":27.41,
 *    "humidity":62.10,"pressure":1012.30,"battery":87}
 *
 * If target_size is larger than the natural document, a "pad" field is added
 * so the payload is exactly target_size bytes (used by payload-size
 * experiments). If it is smaller, the natural size is used.
 *
 * Returns the payload length, or -1 if `cap` is too small.
 */
int telemetry_build(char *buf, size_t cap, const char *device_id, uint32_t seq,
                    uint64_t ts_ms, const SensorData_t *d, size_t target_size);

#endif /* TELEMETRY_H */
