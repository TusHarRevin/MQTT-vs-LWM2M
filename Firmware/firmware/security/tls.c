#include "security/tls.h"

#include <string.h>

#include "common/fw_config.h"
#include "common/fw_log.h"
#include "security/tls_common.h"

#define TAG "tls"

#if FW_HAVE_MBEDTLS

static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    tls_session_t *s = ctx;
    int rc = fw_socket_send(&s->sock, buf, len);

    if (rc >= 0) {
        return rc;
    }
    return rc == FW_ERR_CLOSED ? MBEDTLS_ERR_SSL_CONN_EOF : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

/* mbedTLS: timeout 0 means block forever. */
static int bio_recv_timeout(void *ctx, unsigned char *buf, size_t len, uint32_t timeout)
{
    tls_session_t *s = ctx;
    int rc = fw_socket_recv(&s->sock, buf, len, timeout == 0 ? FW_WAIT_FOREVER : timeout);

    if (rc > 0) {
        return rc;
    }
    switch (rc) {
    case FW_ERR_TIMEOUT: return MBEDTLS_ERR_SSL_TIMEOUT;
    case FW_ERR_CLOSED:  return 0; /* EOF */
    default:             return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }
}

static void session_free(tls_session_t *s)
{
    if (s->initialised) {
        mbedtls_ssl_free(&s->ssl);
        mbedtls_ssl_config_free(&s->conf);
        mbedtls_x509_crt_free(&s->ca);
        mbedtls_x509_crt_free(&s->cert);
        mbedtls_pk_free(&s->key);
        s->initialised = false;
    }
    fw_socket_close(&s->sock);
    s->connected = false;
}

static int fail(tls_session_t *s, const char *what, int mbedtls_rc)
{
    fw_tls_log_error(TAG, what, mbedtls_rc);
    session_free(s);
    return FW_ERR_SECURITY;
}

int tls_connect(tls_session_t *s, const char *host, uint16_t port, const tls_config_t *cfg)
{
    int rc;

    if (s == NULL || host == NULL || cfg == NULL) {
        return FW_ERR_PARAM;
    }
    memset(s, 0, sizeof(*s));
    if (cfg->ca_pem == NULL && !FW_TLS_ALLOW_INSECURE) {
        LOG_E(TAG, "no CA configured; refusing unauthenticated TLS");
        return FW_ERR_SECURITY;
    }
    if ((rc = fw_tls_rng_init()) != FW_OK) {
        return rc;
    }

    mbedtls_ssl_init(&s->ssl);
    mbedtls_ssl_config_init(&s->conf);
    mbedtls_x509_crt_init(&s->ca);
    mbedtls_x509_crt_init(&s->cert);
    mbedtls_pk_init(&s->key);
    s->initialised = true;

    if (cfg->ca_pem != NULL) {
        rc = mbedtls_x509_crt_parse(&s->ca, (const unsigned char *)cfg->ca_pem,
                                    strlen(cfg->ca_pem) + 1);
        if (rc < 0) {
            return fail(s, "parse CA", rc);
        }
    } else {
        LOG_W(TAG, "server certificate NOT verified (FW_TLS_ALLOW_INSECURE)");
    }
    if (cfg->client_cert_pem != NULL) {
        rc = mbedtls_x509_crt_parse(&s->cert, (const unsigned char *)cfg->client_cert_pem,
                                    strlen(cfg->client_cert_pem) + 1);
        if (rc != 0) {
            return fail(s, "parse client cert", rc);
        }
        if (cfg->client_key_pem == NULL) {
            LOG_E(TAG, "client cert without key");
            session_free(s);
            return FW_ERR_PARAM;
        }
        rc = mbedtls_pk_parse_key(&s->key, (const unsigned char *)cfg->client_key_pem,
                                  strlen(cfg->client_key_pem) + 1, NULL, 0,
                                  mbedtls_ctr_drbg_random, fw_tls_rng());
        if (rc != 0) {
            return fail(s, "parse client key", rc);
        }
    }

    rc = mbedtls_ssl_config_defaults(&s->conf, MBEDTLS_SSL_IS_CLIENT,
                                     MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc != 0) {
        return fail(s, "config defaults", rc);
    }
    mbedtls_ssl_conf_rng(&s->conf, mbedtls_ctr_drbg_random, fw_tls_rng());
    if (cfg->ca_pem != NULL) {
        mbedtls_ssl_conf_authmode(&s->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_ca_chain(&s->conf, &s->ca, NULL);
    } else {
        mbedtls_ssl_conf_authmode(&s->conf, MBEDTLS_SSL_VERIFY_NONE);
    }
    if (cfg->client_cert_pem != NULL) {
        rc = mbedtls_ssl_conf_own_cert(&s->conf, &s->cert, &s->key);
        if (rc != 0) {
            return fail(s, "own cert", rc);
        }
    }
    mbedtls_ssl_conf_read_timeout(&s->conf, cfg->handshake_timeout_ms ? cfg->handshake_timeout_ms
                                                                      : FW_NET_TIMEOUT_MS);

    if ((rc = mbedtls_ssl_setup(&s->ssl, &s->conf)) != 0) {
        return fail(s, "ssl setup", rc);
    }
    if ((rc = mbedtls_ssl_set_hostname(&s->ssl, host)) != 0) {
        return fail(s, "set hostname", rc);
    }

    rc = fw_socket_connect(&s->sock, PORT_SOCK_TCP, host, port);
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
    LOG_I(TAG, "%s:%u connected (%s, %s)", host, (unsigned)port,
          mbedtls_ssl_get_version(&s->ssl), mbedtls_ssl_get_ciphersuite(&s->ssl));
    return FW_OK;
}

static int tls_send(void *ctx, const uint8_t *data, size_t len)
{
    tls_session_t *s = ctx;
    size_t sent = 0;

    if (!s->connected) {
        return FW_ERR_STATE;
    }
    while (sent < len) {
        int rc = mbedtls_ssl_write(&s->ssl, data + sent, len - sent);
        if (rc > 0) {
            sent += (size_t)rc;
        } else if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
            fw_tls_log_error(TAG, "write", rc);
            return FW_ERR_IO;
        }
    }
    return (int)sent;
}

static int tls_recv(void *ctx, uint8_t *buf, size_t cap, uint32_t timeout_ms)
{
    tls_session_t *s = ctx;

    if (!s->connected) {
        return FW_ERR_STATE;
    }
    /* mbedTLS treats 0 as "forever", so a poll becomes a 1 ms wait. */
    mbedtls_ssl_conf_read_timeout(&s->conf, timeout_ms == FW_WAIT_FOREVER ? 0
                                            : (timeout_ms ? timeout_ms : 1));
    for (;;) {
        int rc = mbedtls_ssl_read(&s->ssl, buf, cap);
        if (rc > 0) {
            return rc;
        }
        switch (rc) {
        case 0:
        case MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY:
            return FW_ERR_CLOSED;
        case MBEDTLS_ERR_SSL_TIMEOUT:
            return FW_ERR_TIMEOUT;
        case MBEDTLS_ERR_SSL_WANT_READ:
        case MBEDTLS_ERR_SSL_WANT_WRITE:
#if defined(MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)
        case MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET: /* TLS 1.3 */
#endif
            continue;
        default:
            fw_tls_log_error(TAG, "read", rc);
            return FW_ERR_IO;
        }
    }
}

void tls_close(tls_session_t *s)
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

int tls_connect(tls_session_t *s, const char *host, uint16_t port, const tls_config_t *cfg)
{
    FW_UNUSED(s); FW_UNUSED(host); FW_UNUSED(port); FW_UNUSED(cfg);
    LOG_E(TAG, "built without mbedTLS (FW_USE_MBEDTLS=OFF)");
    return FW_ERR_UNSUPPORTED;
}

static int tls_send(void *ctx, const uint8_t *data, size_t len)
{
    FW_UNUSED(ctx); FW_UNUSED(data); FW_UNUSED(len);
    return FW_ERR_UNSUPPORTED;
}

static int tls_recv(void *ctx, uint8_t *buf, size_t cap, uint32_t timeout_ms)
{
    FW_UNUSED(ctx); FW_UNUSED(buf); FW_UNUSED(cap); FW_UNUSED(timeout_ms);
    return FW_ERR_UNSUPPORTED;
}

void tls_close(tls_session_t *s)
{
    FW_UNUSED(s);
}

#endif /* FW_HAVE_MBEDTLS */

static void tls_transport_close(void *ctx)
{
    tls_close((tls_session_t *)ctx);
}

void tls_transport(tls_session_t *s, fw_transport_t *out)
{
    out->ctx = s;
    out->send = tls_send;
    out->recv = tls_recv;
    out->close = tls_transport_close;
}
