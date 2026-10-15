#include "protocol/protocol_manager.h"

#include <stdio.h>

#include "common/fw_config.h"
#include "common/fw_log.h"
#include "hal/timer.h"

#define TAG "proto"

static struct {
    protocol_config_t cfg;
    bool initialised;
    bool connected;
    hal_timer_t retry;
    uint32_t backoff_ms;
    const char *subs[FW_MQTT_MAX_SUBS];
    size_t sub_count;
} s;

static int build_topic(char *out, size_t cap, const char *suffix)
{
    int n = snprintf(out, cap, "%s/%s", s.cfg.mqtt_base_topic ? s.cfg.mqtt_base_topic : "", suffix);

    return (n > 0 && (size_t)n < cap) ? FW_OK : FW_ERR_PARAM;
}

static int mqtt_subscribe_one(const char *suffix)
{
    char topic[FW_TOPIC_MAX];

    if (build_topic(topic, sizeof(topic), suffix) != FW_OK) {
        return FW_ERR_PARAM;
    }
    return mqtt_client_subscribe(topic, 1);
}

static int backend_connect(void)
{
    int rc;

    switch (s.cfg.type) {
    case PROTOCOL_MQTT:
        rc = mqtt_client_connect(&s.cfg.mqtt);
        for (size_t i = 0; rc == FW_OK && i < s.sub_count; i++) {
            rc = mqtt_subscribe_one(s.subs[i]);
        }
        return rc;
    case PROTOCOL_LWM2M:
        return lwm2m_client_start(&s.cfg.lwm2m);
    }
    return FW_ERR_PARAM;
}

static int backend_poll(uint32_t budget_ms)
{
    switch (s.cfg.type) {
    case PROTOCOL_MQTT:  return mqtt_client_poll(budget_ms);
    case PROTOCOL_LWM2M: return lwm2m_client_poll(budget_ms);
    }
    return FW_ERR_PARAM;
}

static void backend_disconnect(void)
{
    switch (s.cfg.type) {
    case PROTOCOL_MQTT:  mqtt_client_disconnect(); break;
    case PROTOCOL_LWM2M: lwm2m_client_stop(); break;
    }
}

static void link_lost(int reason)
{
    LOG_W(TAG, "link lost (%d), retry in %lu ms", reason, (unsigned long)s.backoff_ms);
    backend_disconnect();
    s.connected = false;
    hal_timer_start(&s.retry, s.backoff_ms);
    s.backoff_ms = FW_MIN(s.backoff_ms * 2u, (uint32_t)FW_RECONNECT_MAX_MS);
}

int protocol_manager_init(const protocol_config_t *cfg)
{
    if (cfg == NULL) {
        return FW_ERR_PARAM;
    }
    s.cfg = *cfg;
    s.connected = false;
    s.backoff_ms = FW_RECONNECT_MIN_MS;
    hal_timer_start(&s.retry, 0); /* connect on the first step */
    s.initialised = true;
    LOG_I(TAG, "protocol: %s", cfg->type == PROTOCOL_MQTT ? "MQTT" : "LwM2M");
    return FW_OK;
}

void protocol_manager_step(uint32_t budget_ms)
{
    int rc;

    if (!s.initialised) {
        return;
    }
    if (!s.connected) {
        if (!hal_timer_expired(&s.retry)) {
            return;
        }
        rc = backend_connect();
        if (rc == FW_OK) {
            s.connected = true;
            s.backoff_ms = FW_RECONNECT_MIN_MS;
        } else {
            link_lost(rc);
        }
        return;
    }
    rc = backend_poll(budget_ms);
    if (rc != FW_OK) {
        link_lost(rc);
    }
}

bool protocol_manager_ready(void)
{
    if (!s.connected) {
        return false;
    }
    switch (s.cfg.type) {
    case PROTOCOL_MQTT:  return mqtt_client_is_connected();
    case PROTOCOL_LWM2M: return lwm2m_client_is_registered();
    }
    return false;
}

int protocol_manager_publish(const protocol_msg_t *msg)
{
    char topic[FW_TOPIC_MAX];
    int rc;

    if (msg == NULL) {
        return FW_ERR_PARAM;
    }
    if (!protocol_manager_ready()) {
        return FW_ERR_STATE;
    }
    switch (s.cfg.type) {
    case PROTOCOL_MQTT:
        if (msg->topic == NULL) {
            return FW_OK;
        }
        if (build_topic(topic, sizeof(topic), msg->topic) != FW_OK) {
            return FW_ERR_PARAM;
        }
        rc = mqtt_client_publish(topic, msg->payload, msg->len, msg->qos);
        if (rc != FW_OK && !mqtt_client_is_connected()) {
            link_lost(rc);
        }
        return rc;
    case PROTOCOL_LWM2M:
        return msg->lwm2m_uri ? lwm2m_client_notify(msg->lwm2m_uri) : FW_OK;
    }
    return FW_ERR_PARAM;
}

int protocol_manager_subscribe(const char *topic)
{
    if (topic == NULL) {
        return FW_ERR_PARAM;
    }
    if (s.sub_count >= FW_MQTT_MAX_SUBS) {
        return FW_ERR_NOMEM;
    }
    s.subs[s.sub_count++] = topic;
    if (s.cfg.type == PROTOCOL_MQTT && protocol_manager_ready()) {
        return mqtt_subscribe_one(topic);
    }
    return FW_OK;
}
