#ifndef LAB_H
#define LAB_H

#include <stdbool.h>
#include <stdint.h>

#include "proc.h"
#include "upstream.h"

#define LAB_MAX_RES 24

typedef struct
{
    /* ---- configuration ---- */
    char bind[64];
    uint16_t http_port, events_port;
    char www[512], bin_dir[512], results[512], ota_dir[512], ca_file[512];
    char mqtt_host[64];
    uint16_t mqtt_port, mqtts_port;
    char leshan_host[64];
    uint16_t leshan_port, coap_port, coaps_port;
    bool leshan_tls;           /* Leshan's REST API over HTTPS (public sandbox) */
    char leshan_ca[512];       /* CA for that */
    char mqtt_tls_name[64];    /* name in the broker's certificate */

    /* ---- --public: shared Internet test servers instead of local ones ---- */
    bool public_mode;
    char id_prefix[32];        /* unique per PC, e.g. "pg-3fa9c2": device ids and the topic root */
    char topic[64];            /* MQTT topic root: "playground" or "playground/pg-3fa9c2" */

    /* ---- STM32 + EC200U board (hardware/stm32l452-ec200u) ---- */
    char hw_port[64];          /* serial port of the ST-LINK VCP, "" = no board */
    unsigned hw_baud;
    /* Where the board reaches the servers: public tunnel addresses that
     * forward to the local Mosquitto / Leshan ports. Empty = not set. */
    char hw_mqtt_host[128], hw_mqtts_host[128], hw_coap_host[128], hw_coaps_host[128];
    uint16_t hw_mqtt_port, hw_mqtts_port, hw_coap_port, hw_coaps_port;
    char hw_tls_name[64];      /* name the broker certificate is checked against */

    lab_mutex_t lock;

    /* ---- MQTT broker, as seen by the lab's own subscriber ---- */
    bool broker_online;
    char broker_version[64];
    long sys_clients, sys_msgs_rx, sys_msgs_tx, sys_pub_rx, sys_pub_tx;
    uint32_t sub_received;  /* telemetry messages delivered to the lab */
    char last_topic[128];
    char last_payload[1024];
    uint64_t last_t;

    /* ---- Leshan, as seen through its REST API / event stream ---- */
    bool leshan_online;
    int leshan_clients;
    char leshan_endpoints[512]; /* comma-separated */
    uint32_t leshan_notifications;
    uint32_t leshan_registrations;
    struct { char path[48]; char value[64]; uint64_t t; } res[LAB_MAX_RES];
} lab_t;

extern lab_t L;

/* Push a JSON object to every dashboard (Server-Sent Events) and the replay buffer. */
void lab_broadcast(const char *json);

void monitor_start(void);
/* Connection to Leshan's web API (HTTPS for the public sandbox). */
upstream_t *leshan_connect(void);
/* Publish via the lab's broker connection (used for device commands). */
int monitor_publish(const char *topic, const char *payload, int qos);
/* Snapshot of broker + Leshan state as JSON members (no braces). */
void monitor_state_json(char *out, size_t cap);
void monitor_reset_counters(void);

#endif /* LAB_H */
