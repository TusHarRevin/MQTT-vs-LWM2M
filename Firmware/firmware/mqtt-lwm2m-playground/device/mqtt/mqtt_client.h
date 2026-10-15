#ifndef MQTT_CLIENT_H
#define MQTT_CLIENT_H

/*
 * Minimal MQTT 3.1.1 client on top of link_t.
 *
 * Paho's MQTTPacket library does the packet encoding/decoding; this file owns
 * the I/O so it can time every exchange, count every byte and report each
 * packet to the lab. QoS 0, 1 and 2 are supported in both directions.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "link.h"

#ifndef MQTT_BUF_SIZE
#define MQTT_BUF_SIZE 20480 /* per direction; lowered for the STM32 build */
#endif

typedef void (*mqtt_on_message_t)(const char *topic, size_t topic_len, const uint8_t *payload,
                                  size_t len, void *user);

typedef struct
{
    const char *client_id;
    const char *username;
    const char *password;
    uint16_t keepalive_s;
    uint32_t timeout_ms;
    mqtt_on_message_t on_message;
    void *user;
} mqtt_cfg_t;

typedef struct
{
    link_t *link;
    mqtt_cfg_t cfg;
    bool connected;
    uint16_t next_id;
    uint64_t last_tx_us;
    bool ping_pending;
    uint64_t ping_sent_us;
    uint8_t tx[MQTT_BUF_SIZE];
    uint8_t rx[MQTT_BUF_SIZE];
    size_t rx_len;
} mqtt_t;

typedef struct
{
    double lat_ms;      /* see bench.h for the per-QoS definition */
    double step_ms[3];  /* QoS 2: PUBREC, PUBCOMP; QoS 1: PUBACK */
    uint32_t tx_bytes;  /* wire bytes for the whole exchange */
    uint32_t rx_bytes;
    uint16_t packet_id;
} mqtt_pub_result_t;

#define MQTT_OK 0
#define MQTT_ERR (-1)
#define MQTT_TIMEOUT (-2)
#define MQTT_REFUSED (-3)

/* Sends CONNECT and waits for CONNACK. *lat_ms = CONNECT -> CONNACK. */
int mqtt_connect(mqtt_t *m, link_t *link, const mqtt_cfg_t *cfg, double *lat_ms);
/* qos 0 with probe=true sends a PINGREQ right after the PUBLISH and waits for
 * PINGRESP to time broker receipt. */
int mqtt_publish(mqtt_t *m, const char *topic, const uint8_t *payload, size_t len, int qos,
                 bool probe, const char *detail, mqtt_pub_result_t *res);
int mqtt_subscribe(mqtt_t *m, const char *topic, int qos, double *lat_ms);
/* Handles incoming packets and keep-alive for up to timeout_ms. */
int mqtt_poll(mqtt_t *m, uint32_t timeout_ms);
void mqtt_disconnect(mqtt_t *m);

#endif /* MQTT_CLIENT_H */
