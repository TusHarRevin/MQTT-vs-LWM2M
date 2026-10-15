#include "application/telemetry.h"

#include <stdarg.h>
#include <stdio.h>

#include "application/sensor.h"
#include "common/fw_log.h"
#include "hal/power.h"
#include "hal/timer.h"
#include "protocol/protocol_manager.h"

#define TAG "telemetry"

#define TELEMETRY_JSON_MAX 256

static hal_timer_t s_timer;

typedef struct {
    char buf[TELEMETRY_JSON_MAX];
    size_t len;
    bool overflow;
} json_buf_t;

static void json_append(json_buf_t *j, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (j->overflow) {
        return;
    }
    va_start(ap, fmt);
    n = vsnprintf(j->buf + j->len, sizeof(j->buf) - j->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(j->buf) - j->len) {
        j->overflow = true;
        return;
    }
    j->len += (size_t)n;
}

int telemetry_init(uint32_t interval_ms)
{
    if (interval_ms == 0) {
        return FW_ERR_PARAM;
    }
    hal_timer_start(&s_timer, interval_ms);
    return FW_OK;
}

void telemetry_set_interval(uint32_t interval_ms)
{
    if (interval_ms == 0) {
        return;
    }
    hal_timer_start(&s_timer, interval_ms);
    LOG_I(TAG, "interval %lu ms", (unsigned long)interval_ms);
}

uint32_t telemetry_interval(void)
{
    return s_timer.period;
}

static void publish_mqtt_frame(void)
{
    json_buf_t j = { .len = 0, .overflow = false };
    protocol_msg_t msg = { 0 };
    int mv = hal_power_battery_mv();
    int rc;

    json_append(&j, "{\"ts\":%lu", (unsigned long)hal_uptime_s());
    for (int i = 0; i < SENSOR_COUNT; i++) {
        float v;
        if (sensor_get((sensor_id_t)i, &v)) {
            json_append(&j, ",\"%s\":%.2f", sensor_info((sensor_id_t)i)->name, (double)v);
        }
    }
    if (mv >= 0) {
        json_append(&j, ",\"battery_mv\":%d", mv);
    }
    json_append(&j, "}");
    if (j.overflow) {
        LOG_E(TAG, "frame exceeds %u bytes", (unsigned)TELEMETRY_JSON_MAX);
        return;
    }

    msg.topic = "telemetry";
    msg.payload = (const uint8_t *)j.buf;
    msg.len = j.len;
    msg.qos = 0;
    rc = protocol_manager_publish(&msg);
    if (rc == FW_OK) {
        LOG_I(TAG, "%s", j.buf);
    } else {
        LOG_W(TAG, "publish failed (%d)", rc);
    }
}

static void notify_lwm2m_resources(void)
{
    for (int i = 0; i < SENSOR_COUNT; i++) {
        protocol_msg_t msg = { 0 };
        msg.lwm2m_uri = sensor_info((sensor_id_t)i)->lwm2m_uri;
        (void)protocol_manager_publish(&msg);
    }
}

void telemetry_step(void)
{
    if (!hal_timer_expired(&s_timer)) {
        return;
    }
    hal_timer_restart(&s_timer);

    if (!protocol_manager_ready()) {
        LOG_D(TAG, "not connected, skipping frame");
        return;
    }
    sensor_sample_all();
    /* Each backend ignores the message kind it doesn't use. */
    publish_mqtt_frame();
    notify_lwm2m_resources();
}
