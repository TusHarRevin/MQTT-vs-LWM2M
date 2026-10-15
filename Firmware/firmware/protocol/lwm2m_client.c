#include "protocol/lwm2m_client.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/fw_config.h"
#include "common/fw_log.h"
#include "hal/timer.h"

#define TAG "lwm2m"

#if FW_HAVE_WAKAAMA

#include "liblwm2m.h"

typedef struct {
    fw_socket_t sock;
    dtls_session_t dtls;
    fw_transport_t tr;
} lw_conn_t;

static struct {
    lwm2m_client_config_t cfg;
    lwm2m_context_t *ctx;
    lw_conn_t conn;
    uint8_t rx[FW_LWM2M_BUF_SIZE];
} c;

/* ---- Wakaama connection callbacks -------------------------------------- */

void *lwm2m_connect_server(uint16_t secObjInstID, void *userData)
{
    int rc;

    FW_UNUSED(secObjInstID);
    FW_UNUSED(userData);
    /* TODO: read the server URI and credentials from Security object
     * instance secObjInstID instead of the static config, so bootstrap and
     * multiple servers work. */
    if (fw_transport_is_open(&c.conn.tr)) {
        return &c.conn;
    }
    if (c.cfg.use_dtls) {
        rc = dtls_connect(&c.conn.dtls, c.cfg.host, c.cfg.port, &c.cfg.dtls);
        if (rc == FW_OK) {
            dtls_transport(&c.conn.dtls, &c.conn.tr);
        }
    } else {
        rc = fw_socket_connect(&c.conn.sock, PORT_SOCK_UDP, c.cfg.host, c.cfg.port);
        if (rc == FW_OK) {
            network_socket_transport(&c.conn.sock, &c.conn.tr);
        }
    }
    return rc == FW_OK ? &c.conn : NULL;
}

void lwm2m_close_connection(void *sessionH, void *userData)
{
    lw_conn_t *conn = sessionH;

    FW_UNUSED(userData);
    if (conn != NULL && fw_transport_is_open(&conn->tr)) {
        fw_transport_close(&conn->tr);
    }
}

uint8_t lwm2m_buffer_send(void *sessionH, uint8_t *buffer, size_t length, void *userData)
{
    lw_conn_t *conn = sessionH;

    FW_UNUSED(userData);
    if (conn == NULL || !fw_transport_is_open(&conn->tr)) {
        return COAP_500_INTERNAL_SERVER_ERROR;
    }
    return fw_transport_send(&conn->tr, buffer, length) == (int)length
               ? COAP_NO_ERROR
               : COAP_500_INTERNAL_SERVER_ERROR;
}

bool lwm2m_session_is_equal(void *session1, void *session2, void *userData)
{
    FW_UNUSED(userData);
    return session1 == session2;
}

/* ---- Wakaama platform callbacks ----------------------------------------
 * If the linker reports further lwm2m_* symbols for your pinned Wakaama
 * revision, add them here. */

void *lwm2m_malloc(size_t s)
{
    return malloc(s);
}

void lwm2m_free(void *p)
{
    free(p);
}

char *lwm2m_strdup(const char *str)
{
    size_t n = strlen(str) + 1;
    char *d = malloc(n);

    if (d != NULL) {
        memcpy(d, str, n);
    }
    return d;
}

int lwm2m_strncmp(const char *s1, const char *s2, size_t n)
{
    return strncmp(s1, s2, n);
}

time_t lwm2m_gettime(void)
{
    return (time_t)hal_uptime_s();
}

#ifdef LWM2M_WITH_LOGS
void lwm2m_printf(const char *format, ...)
{
    char buf[160];
    va_list ap;

    va_start(ap, format);
    vsnprintf(buf, sizeof(buf), format, ap);
    va_end(ap);
    LOG_D("wakaama", "%s", buf);
}
#endif

/* ---- Public API -------------------------------------------------------- */

int lwm2m_client_start(const lwm2m_client_config_t *cfg)
{
    int rc;

    if (cfg == NULL || cfg->endpoint == NULL || cfg->host == NULL) {
        return FW_ERR_PARAM;
    }
    if (cfg->objects == NULL || cfg->object_count == 0) {
        LOG_E(TAG, "no objects: provide at least Security(0), Server(1), Device(3)");
        return FW_ERR_PARAM;
    }
    lwm2m_client_stop();
    c.cfg = *cfg;

    c.ctx = lwm2m_init(NULL);
    if (c.ctx == NULL) {
        return FW_ERR_NOMEM;
    }
    rc = lwm2m_configure(c.ctx, cfg->endpoint, NULL, NULL, cfg->object_count,
                         (lwm2m_object_t **)cfg->objects);
    if (rc != COAP_NO_ERROR) {
        LOG_E(TAG, "configure failed (0x%02x)", rc);
        lwm2m_close(c.ctx);
        c.ctx = NULL;
        return FW_ERR;
    }
    LOG_I(TAG, "started, endpoint '%s' -> %s:%u", cfg->endpoint, cfg->host, (unsigned)cfg->port);
    return FW_OK;
}

int lwm2m_client_poll(uint32_t timeout_ms)
{
    time_t step_s = (time_t)((timeout_ms + 999u) / 1000u);
    uint32_t wait_ms;
    int rc;

    if (c.ctx == NULL) {
        return FW_ERR_STATE;
    }
    rc = lwm2m_step(c.ctx, &step_s);
    if (rc != 0) {
        LOG_W(TAG, "step failed (0x%02x)", rc);
        return FW_ERR;
    }
    if (!fw_transport_is_open(&c.conn.tr)) {
        return FW_OK; /* not connected yet; Wakaama retries on a later step */
    }

    wait_ms = FW_MIN(timeout_ms, (uint32_t)step_s * 1000u);
    rc = fw_transport_recv(&c.conn.tr, c.rx, sizeof(c.rx), wait_ms);
    if (rc > 0) {
        lwm2m_handle_packet(c.ctx, c.rx, (size_t)rc, &c.conn);
    } else if (rc != FW_ERR_TIMEOUT) {
        LOG_W(TAG, "receive failed (%d)", rc);
        return rc;
    }
    return FW_OK;
}

bool lwm2m_client_is_registered(void)
{
    return c.ctx != NULL && c.ctx->state == STATE_READY;
}

int lwm2m_client_notify(const char *uri_path)
{
    lwm2m_uri_t uri;

    if (c.ctx == NULL) {
        return FW_ERR_STATE;
    }
    if (uri_path == NULL || lwm2m_stringToUri(uri_path, strlen(uri_path), &uri) == 0) {
        return FW_ERR_PARAM;
    }
    lwm2m_resource_value_changed(c.ctx, &uri);
    return FW_OK;
}

void lwm2m_client_stop(void)
{
    if (c.ctx != NULL) {
        lwm2m_close(c.ctx); /* deregisters and closes the session */
        c.ctx = NULL;
    }
    lwm2m_close_connection(&c.conn, NULL);
}

#else /* !FW_HAVE_WAKAAMA */

int lwm2m_client_start(const lwm2m_client_config_t *cfg)
{
    FW_UNUSED(cfg);
    LOG_E(TAG, "built without Wakaama (FW_USE_WAKAAMA=OFF)");
    return FW_ERR_UNSUPPORTED;
}

int lwm2m_client_poll(uint32_t timeout_ms)
{
    FW_UNUSED(timeout_ms);
    return FW_ERR_UNSUPPORTED;
}

bool lwm2m_client_is_registered(void)
{
    return false;
}

int lwm2m_client_notify(const char *uri_path)
{
    FW_UNUSED(uri_path);
    return FW_ERR_UNSUPPORTED;
}

void lwm2m_client_stop(void)
{
}

#endif /* FW_HAVE_WAKAAMA */
