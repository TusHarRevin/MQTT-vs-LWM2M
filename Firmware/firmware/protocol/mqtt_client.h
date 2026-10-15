#ifndef PROTOCOL_MQTT_CLIENT_H
#define PROTOCOL_MQTT_CLIENT_H

#include "security/tls.h"

/* Topic is NOT NUL-terminated. Do not publish with QoS 1 from inside the
 * callback (it would re-enter the packet reader). */
typedef void (*mqtt_message_cb)(const char *topic, size_t topic_len,
                                const uint8_t *payload, size_t len, void *user);

/* Strings are referenced, not copied: keep them alive while connected. */
typedef struct {
    const char *host;
    uint16_t port;
    bool use_tls;
    tls_config_t tls;
    const char *client_id;
    const char *username; /* optional */
    const char *password; /* optional */
    uint16_t keepalive_s;
    uint32_t timeout_ms;  /* CONNACK / PUBACK / SUBACK wait */
    mqtt_message_cb on_message;
    void *user;
} mqtt_config_t;

/* Single client instance (MQTT 3.1.1, QoS 0/1). All functions return
 * FW_ERR_UNSUPPORTED when built without Paho. */
int mqtt_client_connect(const mqtt_config_t *cfg);
int mqtt_client_publish(const char *topic, const uint8_t *payload, size_t len, int qos);
int mqtt_client_subscribe(const char *topic, int qos);
/* Services keep-alive and dispatches incoming messages; blocks up to timeout_ms. */
int mqtt_client_poll(uint32_t timeout_ms);
bool mqtt_client_is_connected(void);
void mqtt_client_disconnect(void);

#endif /* PROTOCOL_MQTT_CLIENT_H */
