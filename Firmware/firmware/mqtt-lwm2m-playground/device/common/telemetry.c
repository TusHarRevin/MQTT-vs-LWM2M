#include "telemetry.h"

#include <stdio.h>
#include <string.h>

int telemetry_build(char *buf, size_t cap, const char *device_id, uint32_t seq,
                    uint64_t ts_ms, const SensorData_t *d, size_t target_size)
{
    static const char pad_open[] = ",\"pad\":\"";
    int n = snprintf(buf, cap,
                     "{\"dev\":\"%s\",\"seq\":%u,\"ts\":%llu,\"temperature\":%.2f,"
                     "\"humidity\":%.2f,\"pressure\":%.2f,\"battery\":%u",
                     device_id, (unsigned)seq, (unsigned long long)ts_ms, d->temperature,
                     d->humidity, d->pressure, (unsigned)d->battery);
    size_t len, overhead;

    if (n < 0 || (size_t)n + 2 > cap) {
        return -1;
    }
    len = (size_t)n;
    overhead = sizeof(pad_open) - 1 + 2; /* ,"pad":" ... "} */

    if (target_size > len + overhead) {
        size_t fill = target_size - len - overhead;
        if (target_size + 1 > cap) {
            return -1;
        }
        memcpy(buf + len, pad_open, sizeof(pad_open) - 1);
        len += sizeof(pad_open) - 1;
        for (size_t i = 0; i < fill; i++) {
            buf[len++] = (char)('a' + (i % 26));
        }
        buf[len++] = '"';
    }
    buf[len++] = '}';
    buf[len] = '\0';
    return (int)len;
}
