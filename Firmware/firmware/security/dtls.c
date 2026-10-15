#include "security/dtls.h"

#include <string.h>

#include "common/fw_config.h"
#include "common/fw_log.h"
#include "security/tls_common.h"

#define TAG "dtls"

#if FW_HAVE_MBEDTLS

static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    dtls_session_t *s = ctx;
    int rc = fw_socket_send(&s->sock, buf, len);

    return rc >= 0 ? rc : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

static int bio_recv_timeout(void *ctx, unsigned char *buf, size_t len, uint32_t timeout)
{
    dtls_session_t *s = ctx;
    int rc = fw_socket_recv(&s->sock, buf, len, timeout == 0 ? FW_WAIT_FOREVER : timeout);

    if (rc > 0) {
        return rc;
    }
    return rc == FW_ERR_TIMEOUT ? MBEDTLS_ERR_SSL_TIMEOUT : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

/* mbedTLS DTLS timer callbacks, backed by the HAL software timers. */
static void timer_set(void *ctx, uint32_t int_ms, uint32_t fin_ms)
{
    dtls_session_t *s = ctx;

    if (fin_ms == 0) {
        hal_timer_stop(&s->t_int);
        hal_timer_stop(&s->t_fin);
        return;
    }
    hal_timer_start(&s->t_int, int_ms);
    hal_timer_start(&s->t_fin, fin_ms);
}

static int timer_get(void *ctx)
{
    dtls_session_t *s = ctx;

    if (!s->t_fin.running) {
        return -1;
    }
    if (hal_timer_expired(&s->t_fin)) {
        return 2;
    }
    if (hal_timer_expired(&s->t_int)) {
        return 1;
    }
    return 0;
}

static void session_free(dtls_session_t *s)
{
    if (s->initialised) {
        mbedtls_ssl_free(&s->ssl);
        mbedtls_ssl_config_free(&s->conf);
        mbedtls_x509_crt_free(&s->ca);
        s->initialised = false;
    }
    fw_socket_close(&s->sock);
    s->connected = false;
}

static int fail(dtls_session_t *s, const char *what, int mbedtls_rc)
{
    fw_tls_log_error(TAG, what, mbedtls_rc);
    session_free(s);
    return FW_ERR_SECURITY;
}

int dtls_connect(dtls_session_t *s, const char *host, uint16_t port, const dtls_config_t *cfg)
{
    int rc;

    if (s == NULL || host == NULL || cfg == NULL) {
        return FW_ERR_PARAM;
    }
    memset(s, 0, sizeof(*s));
    if (cfg->psk == NULL && cfg->ca_pem == NULL && !FW_TLS_ALLOW_INSECURE) {
        LOG_E(TAG, "need a PSK or CA");
        return FW_ERR_SECURITY;
    }
    if ((rc = fw_tls_rng_init()) != FW_OK) {
        return rc;
    }

    mbedtls_ssl_init(&s->ssl);
    mbedtls_ssl_config_init(&s->conf);
    mbedtls_x509_crt_init(&s->ca);
    s->initialised = true;

    rc = mbedtls_ssl_config_defaults(&s->conf, MBEDTLS_SSL_IS_CLIENT,
                                     MBEDTLS_SSL_TRANSPORT_DATAGRAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc != 0) {
        return fail(s, "config defaults", rc);
    }
    mbedtls_ssl_conf_rng(&s->conf, mbedtls_ctr_drbg_random, fw_tls_rng());

    if (cfg->psk != NULL) {
        if (cfg->psk_identity == NULL) {
            LOG_E(TAG, "PSK without identity");
            session_free(s);
            return FW_ERR_PARAM;
        }
        rc = mbedtls_ssl_conf_psk(&s->conf, cfg->psk, cfg->psk_len,
                                  (const unsigned char *)cfg->psk_identity,
                                  strlen(cfg->psk_identity));
        if (rc != 0) {
            return fail(s, "conf psk", rc);
        }
        mbedtls_ssl_conf_authmode(&s->conf, MBEDTLS_SSL_VERIFY_NONE); /* PSK authenticates */
    } else if (cfg->ca_pem != NULL) {
        rc = mbedtls_x509_crt_parse(&s->ca, (const unsigned char *)cfg->ca_pem,
                                    strlen(cfg->ca_pem) + 1);
        if (rc < 0) {
            return fail(s, "parse CA", rc);
        }
        mbedtls_ssl_conf_ca_chain(&s->conf, &s->ca, NULL);
        mbedtls_ssl_conf_authmode(&s->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    } else {
        LOG_W(TAG, "server NOT authenticated (FW_TLS_ALLOW_INSECURE)");
        mbedtls_ssl_conf_authmode(&s->conf, MBEDTLS_SSL_VERIFY_NONE);
    }
    mbedtls_ssl_conf_read_timeout(&s->conf, cfg->handshake_timeout_ms ? cfg->handshake_timeout_ms
                                                                      : FW_NET_TIMEOUT_MS);

    if ((rc = mbedtls_ssl_setup(&s->ssl, &s->conf)) != 0) {
        return fail(s, "ssl setup", rc);
    }
    if ((rc = mbedtls_ssl_set_hostname(&s->ssl, host)) != 0) {
        return fail(s, "set hostname", rc);
    }
    mbedtls_ssl_set_timer_cb(&s->ssl, s, timer_set, timer_get);

    rc = fw_socket_connect(&s->sock, PORT_SOCK_UDP, host, port);
    if (rc != FW_OK) {
        session_free(s);
        return rc;
    }
    mbedtls_ssl_set_bio(&s->ssl, s, bio_send, NULL, bio_recv_timeout);

    while ((rc = mbedtls_ssl_handshake(&s->ssl)) != 0) {
        if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
            return fail(s, "handshake", rc);
        }
    }
    s->connected = true;
    LOG_I(TAG, "%s:%u connected (%s)", host, (unsigned)port, mbedtls_ssl_get_ciphersuite(&s->ssl));
    return FW_OK;
}

static int dtls_send(void *ctx, const uint8_t *data, size_t len)
{
    dtls_session_t *s = ctx;
    int rc;

    if (!s->connected) {
        return FW_ERR_STATE;
    }
    do {
        rc = mbedtls_ssl_write(&s->ssl, data, len);
    } while (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE);
    if (rc < 0) {
        fw_tls_log_error(TAG, "write", rc);
        return FW_ERR_IO;
    }
    return rc;
}

static int dtls_recv(void *ctx, uint8_t *buf, size_t cap, uint32_t timeout_ms)
{
    dtls_session_t *s = ctx;

    if (!s->connected) {
        return FW_ERR_STATE;
    }
    mbedtls_ssl_conf_read_timeout(&s->conf, timeout_ms == FW_WAIT_FOREVER ? 0
                                            : (timeout_ms ? timeout_ms : 1));
    for (;;) {
        int rc = mbedtls_ssl_read(&s->ssl, buf, cap);
        if (rc > 0) {
            return rc;
        }
        switch (rc) {
        case MBEDTLS_ERR_SSL_TIMEOUT:
            return FW_ERR_TIMEOUT;
        case MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY:
            return FW_ERR_CLOSED;
        case 0:
        case MBEDTLS_ERR_SSL_WANT_READ:
        case MBEDTLS_ERR_SSL_WANT_WRITE:
            continue;
        default:
            fw_tls_log_error(TAG, "read", rc);
            return FW_ERR_IO;
        }
    }
}

void dtls_close(dtls_session_t *s)
{
    if (s == NULL) {
        return;
    }
    if (s->connected) {
        (void)mbedtls_ssl_close_notify(&s->ssl);
    }
    session_free(s);
}

#else /* !FW_HAVE_MBEDTLS */

int dtls_connect(dtls_session_t *s, const char *host, uint16_t port, const dtls_config_t *cfg)
{
    FW_UNUSED(s); FW_UNUSED(host); FW_UNUSED(port); FW_UNUSED(cfg);
    LOG_E(TAG, "built without mbedTLS (FW_USE_MBEDTLS=OFF)");
    return FW_ERR_UNSUPPORTED;
}

static int dtls_send(void *ctx, const uint8_t *data, size_t len)
{
    FW_UNUSED(ctx); FW_UNUSED(data); FW_UNUSED(len);
    return FW_ERR_UNSUPPORTED;
}

static int dtls_recv(void *ctx, uint8_t *buf, size_t cap, uint32_t timeout_ms)
{
    FW_UNUSED(ctx); FW_UNUSED(buf); FW_UNUSED(cap); FW_UNUSED(timeout_ms);
    return FW_ERR_UNSUPPORTED;
}

void dtls_close(dtls_session_t *s)
{
    FW_UNUSED(s);
}

#endif /* FW_HAVE_MBEDTLS */

static void dtls_transport_close(void *ctx)
{
    dtls_close((dtls_session_t *)ctx);
}

void dtls_transport(dtls_session_t *s, fw_transport_t *out)
{
    out->ctx = s;
    out->send = dtls_send;
    out->recv = dtls_recv;
    out->close = dtls_transport_close;
}
