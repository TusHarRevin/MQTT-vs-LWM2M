#ifndef SECURITY_DTLS_H
#define SECURITY_DTLS_H

#include "hal/timer.h"
#include "network/network.h"

#if FW_HAVE_MBEDTLS
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#endif

/* DTLS 1.2 client. PSK is the usual choice for LwM2M; certificate mode is
 * supported for server authentication only. */
typedef struct {
    const uint8_t *psk;
    size_t psk_len;
    const char *psk_identity;
    const char *ca_pem;  /* used when psk == NULL */
    uint32_t handshake_timeout_ms;
} dtls_config_t;

typedef struct {
    fw_socket_t sock;
#if FW_HAVE_MBEDTLS
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt ca;
#endif
    hal_timer_t t_int; /* mbedTLS retransmission timers */
    hal_timer_t t_fin;
    bool initialised;
    bool connected;
} dtls_session_t;

/* UDP connect + DTLS handshake. FW_ERR_UNSUPPORTED without mbedTLS. */
int dtls_connect(dtls_session_t *s, const char *host, uint16_t port, const dtls_config_t *cfg);
void dtls_transport(dtls_session_t *s, fw_transport_t *out);
void dtls_close(dtls_session_t *s);

#endif /* SECURITY_DTLS_H */
