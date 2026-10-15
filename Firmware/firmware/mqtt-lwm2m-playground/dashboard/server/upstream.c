#include "upstream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "link.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "pal.h"
#include "proc.h"
#include "psa/crypto.h"

struct upstream
{
    pal_sock_t sock;
    bool tls;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt ca;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
};

/* mbedTLS is built without MBEDTLS_THREADING_C: handshakes are serialised,
 * and TLS 1.2 is used so the session itself only touches per-connection
 * state (TLS 1.3 goes through PSA's global key store). */
static lab_mutex_t hs_lock;

void upstream_init(void)
{
    mutex_init(&hs_lock);
    psa_crypto_init();
}

static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    upstream_t *u = ctx;
    return pal_sock_send_all(u->sock, buf, len) == 0 ? (int)len : MBEDTLS_ERR_NET_SEND_FAILED;
}

static int bio_recv_timeout(void *ctx, unsigned char *buf, size_t len, uint32_t timeout)
{
    upstream_t *u = ctx;
    int n = pal_sock_recv(u->sock, buf, len, timeout ? timeout : 3600000u);
    if (n == 0) {
        return MBEDTLS_ERR_SSL_TIMEOUT;
    }
    if (n == PAL_CLOSED) {
        return 0; /* EOF */
    }
    return n < 0 ? MBEDTLS_ERR_NET_RECV_FAILED : n;
}

static void tls_free(upstream_t *u)
{
    mbedtls_ssl_free(&u->ssl);
    mbedtls_ssl_config_free(&u->conf);
    mbedtls_x509_crt_free(&u->ca);
    mbedtls_ctr_drbg_free(&u->drbg);
    mbedtls_entropy_free(&u->entropy);
}

static int tls_start(upstream_t *u, const char *host, const char *ca_file)
{
    size_t pem_len = 0;
    char *pem = ca_file ? link_read_file(ca_file, &pem_len) : NULL;
    int rc;

    mbedtls_ssl_init(&u->ssl);
    mbedtls_ssl_config_init(&u->conf);
    mbedtls_x509_crt_init(&u->ca);
    mbedtls_entropy_init(&u->entropy);
    mbedtls_ctr_drbg_init(&u->drbg);

    rc = pem ? mbedtls_x509_crt_parse(&u->ca, (const unsigned char *)pem, pem_len + 1) : -1;
    free(pem);
    if (rc < 0) {
        fprintf(stderr, "[lab] cannot load CA file %s\n", ca_file ? ca_file : "(none)");
        return -1;
    }
    rc = mbedtls_ctr_drbg_seed(&u->drbg, mbedtls_entropy_func, &u->entropy, (const unsigned char *)"pg-lab", 6);
    if (rc == 0) {
        rc = mbedtls_ssl_config_defaults(&u->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                         MBEDTLS_SSL_PRESET_DEFAULT);
    }
    if (rc != 0) {
        return -1;
    }
    mbedtls_ssl_conf_max_tls_version(&u->conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_rng(&u->conf, mbedtls_ctr_drbg_random, &u->drbg);
    mbedtls_ssl_conf_ca_chain(&u->conf, &u->ca, NULL);
    mbedtls_ssl_conf_authmode(&u->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_read_timeout(&u->conf, 10000);
    if (mbedtls_ssl_setup(&u->ssl, &u->conf) != 0 || mbedtls_ssl_set_hostname(&u->ssl, host) != 0) {
        return -1;
    }
    mbedtls_ssl_set_bio(&u->ssl, u, bio_send, NULL, bio_recv_timeout);

    mutex_lock(&hs_lock);
    do {
        rc = mbedtls_ssl_handshake(&u->ssl);
    } while (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE);
    mutex_unlock(&hs_lock);
    if (rc != 0) {
        char err[96];
        mbedtls_strerror(rc, err, sizeof(err));
        fprintf(stderr, "[lab] TLS to %s failed: %s\n", host, err);
        return -1;
    }
    return 0;
}

upstream_t *upstream_open(const char *host, uint16_t port, bool tls, const char *ca_file)
{
    upstream_t *u = calloc(1, sizeof(*u));
    if (u == NULL) {
        return NULL;
    }
    if (pal_sock_connect(&u->sock, false, host, port) != 0) {
        free(u);
        return NULL;
    }
    u->tls = tls;
    if (tls && tls_start(u, host, ca_file) != 0) {
        tls_free(u);
        pal_sock_close(u->sock);
        free(u);
        return NULL;
    }
    return u;
}

int upstream_send_all(upstream_t *u, const void *data, size_t len)
{
    const unsigned char *p = data;
    if (!u->tls) {
        return pal_sock_send_all(u->sock, data, len) == 0 ? 0 : -1;
    }
    while (len > 0) {
        int rc = mbedtls_ssl_write(&u->ssl, p, len);
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
            continue;
        }
        if (rc <= 0) {
            return -1;
        }
        p += rc;
        len -= (size_t)rc;
    }
    return 0;
}

int upstream_recv(upstream_t *u, void *buf, size_t cap, uint32_t timeout_ms)
{
    int rc;
    if (!u->tls) {
        rc = pal_sock_recv(u->sock, buf, cap, timeout_ms);
        return rc > 0 ? rc : (rc == 0 ? 0 : -1);
    }
    mbedtls_ssl_conf_read_timeout(&u->conf, timeout_ms ? timeout_ms : 1);
    for (;;) {
        rc = mbedtls_ssl_read(&u->ssl, buf, cap);
        if (rc > 0) {
            return rc;
        }
        if (rc == MBEDTLS_ERR_SSL_TIMEOUT) {
            return 0;
        }
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
            continue;
        }
        return -1; /* close_notify, EOF or error */
    }
}

void upstream_close(upstream_t *u)
{
    if (u == NULL) {
        return;
    }
    if (u->tls) {
        mbedtls_ssl_close_notify(&u->ssl);
        tls_free(u);
    }
    pal_sock_close(u->sock);
    free(u);
}
