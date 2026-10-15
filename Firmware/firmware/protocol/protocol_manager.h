#ifndef PROTOCOL_MANAGER_H
#define PROTOCOL_MANAGER_H

#include "protocol/lwm2m_client.h"
#include "protocol/mqtt_client.h"

typedef enum { PROTOCOL_MQTT, PROTOCOL_LWM2M } protocol_type_t;

typedef struct {
    protocol_type_t type;
    mqtt_config_t mqtt;
    const char *mqtt_base_topic; /* message topics are "<base>/<topic>" */
    lwm2m_client_config_t lwm2m;
} protocol_config_t;

/* One message, described for every backend. Each backend uses its own field
 * and silently ignores messages that don't carry it. */
typedef struct {
    const char *topic;     /* MQTT: suffix appended to the base topic */
    const char *lwm2m_uri; /* LwM2M: resource to mark as changed */
    const uint8_t *payload;
    size_t len;
    int qos;
} protocol_msg_t;

int protocol_manager_init(const protocol_config_t *cfg);
/* Connects / reconnects with exponential backoff and services the active
 * backend. Blocks up to budget_ms while connected. */
void protocol_manager_step(uint32_t budget_ms);
bool protocol_manager_ready(void);
int protocol_manager_publish(const protocol_msg_t *msg);
/* MQTT only: subscribe to "<base>/<topic>", re-applied after every reconnect.
 * The string is referenced, not copied. */
int protocol_manager_subscribe(const char *topic);

#endif /* PROTOCOL_MANAGER_H */
