#ifndef FW_CONFIG_H
#define FW_CONFIG_H

/* Build-time configuration. Every value can be overridden with -D on the
 * compiler command line (e.g. add_compile_definitions() in CMake). */

/* ---- Protocol selection ------------------------------------------------ */
#define FW_PROTOCOL_MQTT  0
#define FW_PROTOCOL_LWM2M 1

#ifndef FW_PROTOCOL
#define FW_PROTOCOL FW_PROTOCOL_MQTT
#endif

/* ---- MQTT -------------------------------------------------------------- */
#ifndef FW_MQTT_HOST
#define FW_MQTT_HOST "test.mosquitto.org"
#endif
#ifndef FW_MQTT_USE_TLS
#define FW_MQTT_USE_TLS 0
#endif
#ifndef FW_MQTT_PORT
#define FW_MQTT_PORT (FW_MQTT_USE_TLS ? 8883 : 1883)
#endif
#ifndef FW_MQTT_CA_PEM
#define FW_MQTT_CA_PEM NULL /* NUL-terminated PEM string */
#endif
#ifndef FW_MQTT_KEEPALIVE_S
#define FW_MQTT_KEEPALIVE_S 60
#endif
#ifndef FW_MQTT_BUF_SIZE
#define FW_MQTT_BUF_SIZE 1024
#endif
#ifndef FW_MQTT_MAX_SUBS
#define FW_MQTT_MAX_SUBS 4
#endif
#ifndef FW_TOPIC_MAX
#define FW_TOPIC_MAX 128
#endif

/* ---- LwM2M ------------------------------------------------------------- */
#ifndef FW_LWM2M_HOST
#define FW_LWM2M_HOST "leshan.eclipseprojects.io"
#endif
#ifndef FW_LWM2M_USE_DTLS
#define FW_LWM2M_USE_DTLS 0
#endif
#ifndef FW_LWM2M_PORT
#define FW_LWM2M_PORT (FW_LWM2M_USE_DTLS ? 5684 : 5683)
#endif
#ifndef FW_LWM2M_BUF_SIZE
#define FW_LWM2M_BUF_SIZE 1280
#endif

/* ---- Security ---------------------------------------------------------- */
/* 1 = allow TLS without a CA (no server authentication). Development only. */
#ifndef FW_TLS_ALLOW_INSECURE
#define FW_TLS_ALLOW_INSECURE 0
#endif

/* ---- Timing ------------------------------------------------------------ */
#ifndef FW_NET_TIMEOUT_MS
#define FW_NET_TIMEOUT_MS 5000
#endif
#ifndef FW_TELEMETRY_INTERVAL_MS
#define FW_TELEMETRY_INTERVAL_MS 10000
#endif
#ifndef FW_LOOP_BUDGET_MS
#define FW_LOOP_BUDGET_MS 100 /* max time one device_step() blocks on network */
#endif
#ifndef FW_IDLE_MS
#define FW_IDLE_MS 100
#endif
#ifndef FW_RECONNECT_MIN_MS
#define FW_RECONNECT_MIN_MS 1000
#endif
#ifndef FW_RECONNECT_MAX_MS
#define FW_RECONNECT_MAX_MS 60000
#endif
#ifndef FW_NET_RETRY_MS
#define FW_NET_RETRY_MS 5000
#endif

/* ---- Power ------------------------------------------------------------- */
#ifndef FW_BATTERY_LOW_MV
#define FW_BATTERY_LOW_MV 3300
#endif
#ifndef FW_LOW_BATTERY_INTERVAL_FACTOR
#define FW_LOW_BATTERY_INTERVAL_FACTOR 4
#endif

#endif /* FW_CONFIG_H */
