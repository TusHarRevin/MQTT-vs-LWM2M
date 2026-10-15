#ifndef OBJECTS_H
#define OBJECTS_H

/*
 * The LwM2M objects this device exposes (all single-instance, /x/0):
 *
 *   /0     Security                 server URI, security mode, PSK
 *   /1     Server                   lifetime, binding, pmin/pmax
 *   /3     Device                   manufacturer, model, battery (/3/0/9), time
 *   /19    BinaryAppDataContainer   /19/0/0 = telemetry JSON (sized by --payload)
 *   /3303  Temperature (IPSO)       /3303/0/5700 in Cel
 *   /3304  Humidity (IPSO)          /3304/0/5700 in %RH
 *   /3315  Barometer (IPSO)         /3315/0/5700 in hPa
 *
 * Battery note: OMA has no stand-alone "battery" object in the core set;
 * battery level is a Device resource, /3/0/9 (percent). /3/0/13 is
 * Current Time.
 */

#include <stdbool.h>
#include <stdint.h>

#include "liblwm2m.h"
#include "sensor.h"

typedef struct
{
    /* Security / server configuration */
    char server_uri[128];
    bool psk_mode;
    char psk_identity[64];
    uint8_t psk[64];
    size_t psk_len;
    uint16_t short_id;
    int64_t lifetime_s;
    int64_t pmin_s, pmax_s;
    bool notif_storing;

    /* Device */
    char serial[32];
    char fw_version[16];
    char utc_offset[8];
    char timezone[32];
    int64_t time_delta_s; /* written Current Time - real time */

    /* Measurements */
    SensorData_t now;
    float min[3], max[3]; /* temperature, humidity, pressure */
    char app_type[3][32];
    char blob[2048];      /* /19/0/0 content */
    size_t blob_len;

    /* Callbacks into the client (reboot, update trigger) */
    void (*on_execute)(const char *path);
    void (*on_write)(const char *path, const char *value);
} lw_state_t;

extern lw_state_t g_lw;

#define LW_OBJECT_COUNT 7

/* Builds all objects. Returns the array Wakaama wants for lwm2m_configure(). */
lwm2m_object_t **objects_create(void);
void objects_free(void);
/* Push a new sensor reading into the objects (also tracks min/max). */
void objects_update(const SensorData_t *d);

#endif /* OBJECTS_H */
