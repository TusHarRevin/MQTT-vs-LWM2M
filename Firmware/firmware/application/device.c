#include "application/device.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "application/sensor.h"
#include "application/telemetry.h"
#include "common/fw_config.h"
#include "common/fw_log.h"
#include "hal/power.h"
#include "hal/storage.h"
#include "hal/timer.h"
#include "network/network.h"
#include "platform/port.h"
#include "protocol/protocol_manager.h"

#define TAG "device"

#define KEY_DEVICE_ID "device_id"
#define KEY_INTERVAL  "tele_interval"

#define DEVICE_ID_BYTES 8

static struct {
    char id[DEVICE_ID_BYTES * 2 + 1];
    char base_topic[48];
    protocol_config_t proto;
    bool net_up;
    hal_timer_t net_retry;
    uint32_t interval_ms; /* configured interval, before battery scaling */
    bool low_battery;
} d;

static void load_or_create_id(void)
{
    uint8_t raw[DEVICE_ID_BYTES];
    size_t len = 0;

    if (hal_storage_get(KEY_DEVICE_ID, d.id, sizeof(d.id) - 1, &len) == FW_OK && len > 0) {
        d.id[len] = '\0';
        return;
    }
    if (port_entropy(raw, sizeof(raw)) != FW_OK) {
        LOG_W(TAG, "no entropy source, using uptime for ID");
        for (size_t i = 0; i < sizeof(raw); i++) {
            raw[i] = (uint8_t)(hal_millis() >> (8 * (i % 4)));
        }
    }
    for (size_t i = 0; i < sizeof(raw); i++) {
        snprintf(&d.id[i * 2], 3, "%02x", raw[i]);
    }
    if (hal_storage_set(KEY_DEVICE_ID, d.id, strlen(d.id)) != FW_OK) {
        LOG_W(TAG, "could not persist device ID");
    }
}

static uint32_t load_interval(void)
{
    uint32_t v = 0;
    size_t len = 0;

    if (hal_storage_get(KEY_INTERVAL, &v, sizeof(v), &len) == FW_OK && len == sizeof(v) && v > 0) {
        return v;
    }
    return FW_TELEMETRY_INTERVAL_MS;
}

static void apply_interval(void)
{
    uint32_t effective = d.interval_ms;

    if (d.low_battery) {
        effective *= FW_LOW_BATTERY_INTERVAL_FACTOR;
    }
    telemetry_set_interval(effective);
}

/* Commands on "<base>/cmd": "reboot" or "interval=<ms>". */
static void on_message(const char *topic, size_t topic_len, const uint8_t *payload, size_t len,
                       void *user)
{
    char cmd[64];

    FW_UNUSED(user);
    if (len >= sizeof(cmd)) {
        LOG_W(TAG, "command too long");
        return;
    }
    memcpy(cmd, payload, len);
    cmd[len] = '\0';
    LOG_I(TAG, "command on %.*s: %s", (int)topic_len, topic, cmd);

    if (strcmp(cmd, "reboot") == 0) {
        hal_power_reset();
    } else if (strncmp(cmd, "interval=", 9) == 0) {
        unsigned long ms = strtoul(cmd + 9, NULL, 10);
        if (ms < 1000 || ms > 86400000UL) {
            LOG_W(TAG, "interval out of range");
            return;
        }
        d.interval_ms = (uint32_t)ms;
        (void)hal_storage_set(KEY_INTERVAL, &d.interval_ms, sizeof(d.interval_ms));
        apply_interval();
    } else {
        LOG_W(TAG, "unknown command");
    }
}

static void build_protocol_config(void)
{
    protocol_config_t *p = &d.proto;

    memset(p, 0, sizeof(*p));
    p->type = FW_PROTOCOL == FW_PROTOCOL_LWM2M ? PROTOCOL_LWM2M : PROTOCOL_MQTT;

    snprintf(d.base_topic, sizeof(d.base_topic), "devices/%s", d.id);
    p->mqtt_base_topic = d.base_topic;
    p->mqtt.host = FW_MQTT_HOST;
    p->mqtt.port = FW_MQTT_PORT;
    p->mqtt.use_tls = FW_MQTT_USE_TLS;
    p->mqtt.tls.ca_pem = FW_MQTT_CA_PEM;
    p->mqtt.tls.handshake_timeout_ms = FW_NET_TIMEOUT_MS;
    p->mqtt.client_id = d.id;
    p->mqtt.keepalive_s = FW_MQTT_KEEPALIVE_S;
    p->mqtt.timeout_ms = FW_NET_TIMEOUT_MS;
    p->mqtt.on_message = on_message;

    p->lwm2m.endpoint = d.id;
    p->lwm2m.host = FW_LWM2M_HOST;
    p->lwm2m.port = FW_LWM2M_PORT;
    p->lwm2m.use_dtls = FW_LWM2M_USE_DTLS;
    p->lwm2m.dtls.handshake_timeout_ms = FW_NET_TIMEOUT_MS;
    /* TODO: set p->lwm2m.dtls.psk / psk_identity when FW_LWM2M_USE_DTLS=1. */
    /* TODO: create the LwM2M objects (Security, Server, Device, IPSO 3303/3304
     * reading sensor_get()) and assign p->lwm2m.objects / object_count. */
}

int device_init(void)
{
    int rc = port_init();

    if (rc != FW_OK) {
        return rc;
    }
    LOG_I(TAG, "firmware starting");

    load_or_create_id();
    LOG_I(TAG, "device id %s", d.id);

    sensor_init();
    build_protocol_config();
    protocol_manager_init(&d.proto);
    protocol_manager_subscribe("cmd");

    d.interval_ms = load_interval();
    d.low_battery = hal_power_battery_low();
    telemetry_init(d.interval_ms);
    apply_interval();

    d.net_up = network_init() == FW_OK;
    hal_timer_start(&d.net_retry, FW_NET_RETRY_MS);
    return FW_OK;
}

void device_step(void)
{
    bool low;

    if (!d.net_up || !network_is_up()) {
        d.net_up = false;
        if (hal_timer_expired(&d.net_retry)) {
            hal_timer_start(&d.net_retry, FW_NET_RETRY_MS);
            d.net_up = network_init() == FW_OK;
        }
        hal_power_idle(FW_IDLE_MS);
        return;
    }

    low = hal_power_battery_low();
    if (low != d.low_battery) {
        d.low_battery = low;
        LOG_W(TAG, "battery %s", low ? "low: reducing telemetry rate" : "recovered");
        apply_interval();
    }

    protocol_manager_step(FW_LOOP_BUDGET_MS);
    telemetry_step();

    /* While connected, protocol_manager_step() already blocked on the socket. */
    if (!protocol_manager_ready()) {
        hal_power_idle(FW_IDLE_MS);
    }
}

const char *device_id(void)
{
    return d.id;
}
