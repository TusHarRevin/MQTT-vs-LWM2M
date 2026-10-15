#include "objects.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

lw_state_t g_lw;

/* ---- Small table-driven object engine ------------------------------------
 * Every object here is single-instance (/x/0). An object is described by a
 * resource table plus get/set/exec callbacks; read/write/execute/discover
 * are implemented once, generically. This is how one would keep code size
 * down on an MCU too. */

#define OP_R 1
#define OP_W 2
#define OP_E 4

typedef struct
{
    uint16_t id;
    uint8_t ops;
} res_t;

typedef struct
{
    uint16_t oid;
    const res_t *res;
    int nres;
    uint8_t (*get)(uint16_t rid, lwm2m_data_t *d);
    uint8_t (*set)(uint16_t rid, const lwm2m_data_t *d);
    uint8_t (*exec)(uint16_t rid, const uint8_t *buf, int len);
} def_t;

static const res_t *find_res(const def_t *d, uint16_t id)
{
    for (int i = 0; i < d->nres; i++) {
        if (d->res[i].id == id) {
            return &d->res[i];
        }
    }
    return NULL;
}

static uint8_t gen_read(lwm2m_context_t *ctx, uint16_t inst, int *numDataP, lwm2m_data_t **arr,
                        lwm2m_object_t *obj)
{
    const def_t *d = obj->userData;
    (void)ctx;
    if (inst != 0) {
        return COAP_404_NOT_FOUND;
    }
    if (*numDataP == 0) {
        int n = 0;
        for (int i = 0; i < d->nres; i++) {
            n += (d->res[i].ops & OP_R) != 0;
        }
        *arr = lwm2m_data_new(n);
        if (*arr == NULL) {
            return COAP_500_INTERNAL_SERVER_ERROR;
        }
        *numDataP = n;
        n = 0;
        for (int i = 0; i < d->nres; i++) {
            if (d->res[i].ops & OP_R) {
                (*arr)[n++].id = d->res[i].id;
            }
        }
    }
    for (int i = 0; i < *numDataP; i++) {
        const res_t *r = find_res(d, (*arr)[i].id);
        uint8_t rc;
        if (r == NULL) {
            return COAP_404_NOT_FOUND;
        }
        if (!(r->ops & OP_R)) {
            return COAP_405_METHOD_NOT_ALLOWED;
        }
        rc = d->get(r->id, &(*arr)[i]);
        if (rc != COAP_205_CONTENT) {
            return rc;
        }
    }
    return COAP_205_CONTENT;
}

static uint8_t gen_write(lwm2m_context_t *ctx, uint16_t inst, int n, lwm2m_data_t *data,
                         lwm2m_object_t *obj, lwm2m_write_type_t wt)
{
    const def_t *d = obj->userData;
    (void)ctx; (void)wt;
    if (inst != 0) {
        return COAP_404_NOT_FOUND;
    }
    for (int i = 0; i < n; i++) {
        const res_t *r = find_res(d, data[i].id);
        uint8_t rc;
        if (r == NULL) {
            return COAP_404_NOT_FOUND;
        }
        if (!(r->ops & OP_W) || d->set == NULL) {
            return COAP_405_METHOD_NOT_ALLOWED;
        }
        rc = d->set(r->id, &data[i]);
        if (rc != COAP_204_CHANGED) {
            return rc;
        }
    }
    return COAP_204_CHANGED;
}

static uint8_t gen_exec(lwm2m_context_t *ctx, uint16_t inst, uint16_t rid, uint8_t *buf, int len,
                        lwm2m_object_t *obj)
{
    const def_t *d = obj->userData;
    const res_t *r = find_res(d, rid);
    (void)ctx;
    if (inst != 0 || r == NULL) {
        return COAP_404_NOT_FOUND;
    }
    if (!(r->ops & OP_E) || d->exec == NULL) {
        return COAP_405_METHOD_NOT_ALLOWED;
    }
    return d->exec(rid, buf, len);
}

static uint8_t gen_discover(lwm2m_context_t *ctx, uint16_t inst, int *numDataP, lwm2m_data_t **arr,
                            lwm2m_object_t *obj)
{
    const def_t *d = obj->userData;
    (void)ctx;
    if (inst != 0) {
        return COAP_404_NOT_FOUND;
    }
    if (*numDataP == 0) {
        *arr = lwm2m_data_new(d->nres);
        if (*arr == NULL) {
            return COAP_500_INTERNAL_SERVER_ERROR;
        }
        *numDataP = d->nres;
        for (int i = 0; i < d->nres; i++) {
            (*arr)[i].id = d->res[i].id;
        }
        return COAP_205_CONTENT;
    }
    for (int i = 0; i < *numDataP; i++) {
        if (find_res(d, (*arr)[i].id) == NULL) {
            return COAP_404_NOT_FOUND;
        }
    }
    return COAP_205_CONTENT;
}

/* Multiple-instance resource with a single instance 0. */
static uint8_t encode_single_instance(lwm2m_data_t *d, void (*enc)(lwm2m_data_t *, const void *),
                                      const void *arg)
{
    if (d->type == LWM2M_TYPE_MULTIPLE_RESOURCE) {
        for (size_t i = 0; i < d->value.asChildren.count; i++) {
            if (d->value.asChildren.array[i].id != 0) {
                return COAP_404_NOT_FOUND;
            }
            enc(&d->value.asChildren.array[i], arg);
        }
        return COAP_205_CONTENT;
    }
    {
        lwm2m_data_t *sub = lwm2m_data_new(1);
        if (sub == NULL) {
            return COAP_500_INTERNAL_SERVER_ERROR;
        }
        sub->id = 0;
        enc(sub, arg);
        lwm2m_data_encode_instances(sub, 1, d);
    }
    return COAP_205_CONTENT;
}

static void enc_int0(lwm2m_data_t *d, const void *arg)
{
    lwm2m_data_encode_int(*(const int64_t *)arg, d);
}

static void enc_blob(lwm2m_data_t *d, const void *arg)
{
    (void)arg;
    lwm2m_data_encode_opaque((const uint8_t *)g_lw.blob, g_lw.blob_len, d);
}

static bool decode_string(const lwm2m_data_t *d, char *out, size_t cap)
{
    if (d->type != LWM2M_TYPE_STRING && d->type != LWM2M_TYPE_OPAQUE) {
        return false;
    }
    if (d->value.asBuffer.length >= cap) {
        return false;
    }
    memcpy(out, d->value.asBuffer.buffer, d->value.asBuffer.length);
    out[d->value.asBuffer.length] = '\0';
    return true;
}

static void notify_write(uint16_t oid, uint16_t rid, const char *value)
{
    char path[32];
    if (g_lw.on_write) {
        snprintf(path, sizeof(path), "/%u/0/%u", oid, rid);
        g_lw.on_write(path, value);
    }
}

/* ---- /0 Security ---------------------------------------------------------- */

static const res_t sec_res[] = {
    { 0, OP_R }, { 1, OP_R }, { 2, OP_R }, { 3, OP_R }, { 4, OP_R }, { 5, OP_R }, { 10, OP_R }, { 11, OP_R },
};

static uint8_t sec_get(uint16_t rid, lwm2m_data_t *d)
{
    switch (rid) {
    case LWM2M_SECURITY_URI_ID: lwm2m_data_encode_string(g_lw.server_uri, d); break;
    case LWM2M_SECURITY_BOOTSTRAP_ID: lwm2m_data_encode_bool(false, d); break;
    case LWM2M_SECURITY_SECURITY_ID: lwm2m_data_encode_int(g_lw.psk_mode ? 0 : 3, d); break; /* 0 PSK, 3 NoSec */
    case LWM2M_SECURITY_PUBLIC_KEY_ID:
        lwm2m_data_encode_opaque((const uint8_t *)g_lw.psk_identity, strlen(g_lw.psk_identity), d);
        break;
    case LWM2M_SECURITY_SERVER_PUBLIC_KEY_ID: lwm2m_data_encode_opaque(NULL, 0, d); break;
    case LWM2M_SECURITY_SECRET_KEY_ID: lwm2m_data_encode_opaque(g_lw.psk, g_lw.psk_len, d); break;
    case LWM2M_SECURITY_SHORT_SERVER_ID: lwm2m_data_encode_int(g_lw.short_id, d); break;
    case LWM2M_SECURITY_HOLD_OFF_ID: lwm2m_data_encode_int(0, d); break;
    default: return COAP_404_NOT_FOUND;
    }
    return COAP_205_CONTENT;
}

static const def_t sec_def = { LWM2M_SECURITY_OBJECT_ID, sec_res, sizeof(sec_res) / sizeof(sec_res[0]),
                               sec_get, NULL, NULL };

/* ---- /1 Server ------------------------------------------------------------ */

static const res_t srv_res[] = {
    { 0, OP_R }, { 1, OP_R | OP_W }, { 2, OP_R | OP_W }, { 3, OP_R | OP_W },
    { 6, OP_R | OP_W }, { 7, OP_R | OP_W }, { 8, OP_E },
};

static uint8_t srv_get(uint16_t rid, lwm2m_data_t *d)
{
    switch (rid) {
    case 0: lwm2m_data_encode_int(g_lw.short_id, d); break;
    case 1: lwm2m_data_encode_int(g_lw.lifetime_s, d); break;
    case 2: lwm2m_data_encode_int(g_lw.pmin_s, d); break;
    case 3: lwm2m_data_encode_int(g_lw.pmax_s, d); break;
    case 6: lwm2m_data_encode_bool(g_lw.notif_storing, d); break;
    case 7: lwm2m_data_encode_string("U", d); break;
    default: return COAP_404_NOT_FOUND;
    }
    return COAP_205_CONTENT;
}

static uint8_t srv_set(uint16_t rid, const lwm2m_data_t *d)
{
    int64_t v;
    char buf[32];
    switch (rid) {
    case 1: case 2: case 3:
        if (lwm2m_data_decode_int(d, &v) != 1 || v < 0) {
            return COAP_400_BAD_REQUEST;
        }
        if (rid == 1) g_lw.lifetime_s = v;
        if (rid == 2) g_lw.pmin_s = v;
        if (rid == 3) g_lw.pmax_s = v;
        snprintf(buf, sizeof(buf), "%lld", (long long)v);
        break;
    case 6: {
        bool b;
        if (lwm2m_data_decode_bool(d, &b) != 1) {
            return COAP_400_BAD_REQUEST;
        }
        g_lw.notif_storing = b;
        snprintf(buf, sizeof(buf), "%s", b ? "true" : "false");
        break;
    }
    case 7:
        if (!decode_string(d, buf, sizeof(buf)) || strcmp(buf, "U") != 0) {
            return COAP_400_BAD_REQUEST; /* only UDP binding is implemented */
        }
        break;
    default:
        return COAP_404_NOT_FOUND;
    }
    notify_write(1, rid, buf);
    return COAP_204_CHANGED;
}

static uint8_t srv_exec(uint16_t rid, const uint8_t *buf, int len)
{
    (void)buf; (void)len;
    if (rid == 8 && g_lw.on_execute) {
        g_lw.on_execute("/1/0/8"); /* Registration Update Trigger */
        return COAP_204_CHANGED;
    }
    return COAP_405_METHOD_NOT_ALLOWED;
}

static const def_t srv_def = { LWM2M_SERVER_OBJECT_ID, srv_res, sizeof(srv_res) / sizeof(srv_res[0]),
                               srv_get, srv_set, srv_exec };

/* ---- /3 Device ------------------------------------------------------------ */

static const res_t dev_res[] = {
    { 0, OP_R }, { 1, OP_R }, { 2, OP_R }, { 3, OP_R }, { 4, OP_E }, { 9, OP_R }, { 11, OP_R },
    { 13, OP_R | OP_W }, { 14, OP_R | OP_W }, { 15, OP_R | OP_W }, { 16, OP_R }, { 17, OP_R },
    { 18, OP_R }, { 19, OP_R }, { 20, OP_R },
};

static uint8_t dev_get(uint16_t rid, lwm2m_data_t *d)
{
    static const int64_t no_error = 0;
    switch (rid) {
    case 0: lwm2m_data_encode_string("Playground Labs (simulated)", d); break;
    case 1: lwm2m_data_encode_string("PG-SIM-1", d); break;
    case 2: lwm2m_data_encode_string(g_lw.serial, d); break;
    case 3: lwm2m_data_encode_string(g_lw.fw_version, d); break;
    case 9: lwm2m_data_encode_int(g_lw.now.battery, d); break;
    case 11: return encode_single_instance(d, enc_int0, &no_error);
    case 13: lwm2m_data_encode_int((int64_t)time(NULL) + g_lw.time_delta_s, d); break;
    case 14: lwm2m_data_encode_string(g_lw.utc_offset, d); break;
    case 15: lwm2m_data_encode_string(g_lw.timezone, d); break;
    case 16: lwm2m_data_encode_string("U", d); break;
    case 17: lwm2m_data_encode_string("Simulated environmental sensor", d); break;
    case 18: lwm2m_data_encode_string("PC simulation", d); break;
    case 19: lwm2m_data_encode_string("mqtt-lwm2m-playground", d); break;
    /* 0 normal, 1 charging, 2 charge complete, 3 damaged, 4 low */
    case 20: lwm2m_data_encode_int(g_lw.now.battery < 20 ? 4 : 0, d); break;
    default: return COAP_404_NOT_FOUND;
    }
    return COAP_205_CONTENT;
}

static uint8_t dev_set(uint16_t rid, const lwm2m_data_t *d)
{
    char buf[40];
    switch (rid) {
    case 13: {
        int64_t v;
        if (lwm2m_data_decode_int(d, &v) != 1) {
            return COAP_400_BAD_REQUEST;
        }
        g_lw.time_delta_s = v - (int64_t)time(NULL);
        snprintf(buf, sizeof(buf), "%lld", (long long)v);
        break;
    }
    case 14:
        if (!decode_string(d, g_lw.utc_offset, sizeof(g_lw.utc_offset))) {
            return COAP_400_BAD_REQUEST;
        }
        snprintf(buf, sizeof(buf), "%s", g_lw.utc_offset);
        break;
    case 15:
        if (!decode_string(d, g_lw.timezone, sizeof(g_lw.timezone))) {
            return COAP_400_BAD_REQUEST;
        }
        snprintf(buf, sizeof(buf), "%s", g_lw.timezone);
        break;
    default:
        return COAP_405_METHOD_NOT_ALLOWED;
    }
    notify_write(3, rid, buf);
    return COAP_204_CHANGED;
}

static uint8_t dev_exec(uint16_t rid, const uint8_t *buf, int len)
{
    (void)buf; (void)len;
    if (rid == 4 && g_lw.on_execute) {
        g_lw.on_execute("/3/0/4"); /* Reboot (simulated) */
        return COAP_204_CHANGED;
    }
    return COAP_405_METHOD_NOT_ALLOWED;
}

static const def_t dev_def = { 3, dev_res, sizeof(dev_res) / sizeof(dev_res[0]), dev_get, dev_set, dev_exec };

/* ---- /19 BinaryAppDataContainer ------------------------------------------- */

static const res_t bin_res[] = { { 0, OP_R }, { 3, OP_R }, { 4, OP_R } };

static uint8_t bin_get(uint16_t rid, lwm2m_data_t *d)
{
    switch (rid) {
    case 0: return encode_single_instance(d, enc_blob, NULL);
    case 3: lwm2m_data_encode_string("telemetry", d); break;
    case 4: lwm2m_data_encode_string("application/json", d); break;
    default: return COAP_404_NOT_FOUND;
    }
    return COAP_205_CONTENT;
}

static const def_t bin_def = { 19, bin_res, sizeof(bin_res) / sizeof(bin_res[0]), bin_get, NULL, NULL };

/* ---- IPSO sensors: 3303 Temperature, 3304 Humidity, 3315 Barometer -------- */

static const res_t ipso_res[] = {
    { 5700, OP_R }, { 5701, OP_R }, { 5601, OP_R }, { 5602, OP_R }, { 5603, OP_R },
    { 5604, OP_R }, { 5605, OP_E }, { 5750, OP_R | OP_W },
};

static const struct {
    const char *units;
    float lo, hi;
} ipso_info[3] = {
    { "Cel", SENSOR_TEMP_MIN, SENSOR_TEMP_MAX },
    { "%RH", SENSOR_HUM_MIN, SENSOR_HUM_MAX },
    { "hPa", SENSOR_PRES_MIN, SENSOR_PRES_MAX },
};

static float sensor_value(int idx)
{
    return idx == 0 ? g_lw.now.temperature : (idx == 1 ? g_lw.now.humidity : g_lw.now.pressure);
}

static uint8_t ipso_get(int idx, uint16_t rid, lwm2m_data_t *d)
{
    switch (rid) {
    case 5700: lwm2m_data_encode_float(sensor_value(idx), d); break;
    case 5701: lwm2m_data_encode_string(ipso_info[idx].units, d); break;
    case 5601: lwm2m_data_encode_float(g_lw.min[idx], d); break;
    case 5602: lwm2m_data_encode_float(g_lw.max[idx], d); break;
    case 5603: lwm2m_data_encode_float(ipso_info[idx].lo, d); break;
    case 5604: lwm2m_data_encode_float(ipso_info[idx].hi, d); break;
    case 5750: lwm2m_data_encode_string(g_lw.app_type[idx], d); break;
    default: return COAP_404_NOT_FOUND;
    }
    return COAP_205_CONTENT;
}

static uint8_t ipso_set(int idx, uint16_t oid, uint16_t rid, const lwm2m_data_t *d)
{
    if (rid != 5750 || !decode_string(d, g_lw.app_type[idx], sizeof(g_lw.app_type[idx]))) {
        return COAP_400_BAD_REQUEST;
    }
    notify_write(oid, rid, g_lw.app_type[idx]);
    return COAP_204_CHANGED;
}

static uint8_t ipso_exec(int idx, uint16_t oid, uint16_t rid)
{
    char path[32];
    if (rid != 5605) {
        return COAP_405_METHOD_NOT_ALLOWED;
    }
    g_lw.min[idx] = g_lw.max[idx] = sensor_value(idx); /* Reset Min and Max Measured Values */
    if (g_lw.on_execute) {
        snprintf(path, sizeof(path), "/%u/0/5605", oid);
        g_lw.on_execute(path);
    }
    return COAP_204_CHANGED;
}

#define IPSO_FUNCS(N, IDX, OID)                                                                    \
    static uint8_t get_##N(uint16_t r, lwm2m_data_t *d) { return ipso_get(IDX, r, d); }           \
    static uint8_t set_##N(uint16_t r, const lwm2m_data_t *d) { return ipso_set(IDX, OID, r, d); } \
    static uint8_t exe_##N(uint16_t r, const uint8_t *b, int l)                                   \
    {                                                                                              \
        (void)b; (void)l;                                                                          \
        return ipso_exec(IDX, OID, r);                                                             \
    }                                                                                              \
    static const def_t def_##N = { OID, ipso_res, sizeof(ipso_res) / sizeof(ipso_res[0]),         \
                                   get_##N, set_##N, exe_##N };

IPSO_FUNCS(temp, 0, 3303)
IPSO_FUNCS(hum, 1, 3304)
IPSO_FUNCS(pres, 2, 3315)

/* ---- Construction ---------------------------------------------------------- */

static lwm2m_object_t *objects[LW_OBJECT_COUNT];

static lwm2m_object_t *make(const def_t *def)
{
    lwm2m_object_t *o = lwm2m_malloc(sizeof(*o));
    lwm2m_list_t *inst = lwm2m_malloc(sizeof(*inst));
    if (o == NULL || inst == NULL) {
        return NULL;
    }
    memset(o, 0, sizeof(*o));
    memset(inst, 0, sizeof(*inst));
    inst->id = 0;
    o->objID = def->oid;
    o->instanceList = inst;
    o->readFunc = gen_read;
    o->writeFunc = gen_write;
    o->executeFunc = gen_exec;
    o->discoverFunc = gen_discover;
    o->userData = (void *)def;
    return o;
}

lwm2m_object_t **objects_create(void)
{
    const def_t *defs[LW_OBJECT_COUNT] = { &sec_def, &srv_def, &dev_def, &bin_def,
                                           &def_temp, &def_hum, &def_pres };
    for (int i = 0; i < LW_OBJECT_COUNT; i++) {
        objects[i] = make(defs[i]);
        if (objects[i] == NULL) {
            return NULL;
        }
    }
    for (int i = 0; i < 3; i++) {
        g_lw.min[i] = g_lw.max[i] = sensor_value(i);
        snprintf(g_lw.app_type[i], sizeof(g_lw.app_type[i]), "%s",
                 i == 0 ? "room temperature" : (i == 1 ? "room humidity" : "barometer"));
    }
    return objects;
}

void objects_free(void)
{
    for (int i = 0; i < LW_OBJECT_COUNT; i++) {
        if (objects[i]) {
            lwm2m_free(objects[i]->instanceList);
            lwm2m_free(objects[i]);
            objects[i] = NULL;
        }
    }
}

void objects_update(const SensorData_t *d)
{
    g_lw.now = *d;
    for (int i = 0; i < 3; i++) {
        float v = sensor_value(i);
        if (v < g_lw.min[i]) g_lw.min[i] = v;
        if (v > g_lw.max[i]) g_lw.max[i] = v;
    }
}
