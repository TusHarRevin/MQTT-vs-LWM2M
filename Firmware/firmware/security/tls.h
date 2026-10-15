#ifndef SECURITY_TLS_H
#define SECURITY_TLS_H

#include "network/network.h"

#if FW_HAVE_MBEDTLS
#include "mbedtls/pk.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#endif

typedef struct {
    const char *ca_pem;          /* NUL-terminated PEM; NULL = no server auth (needs FW_TLS_ALLOW_INSECURE) */
    const char *client_cert_pem; /* optional, for mutual TLS */
    const char *client_key_pem;  /* required if client_cert_pem is set */
    uint32_t handshake_timeout_ms;
} tls_config_t;

typedef struct {
    fw_socket_t sock;
#if FW_HAVE_MBEDTLS
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt ca;
    mbedtls_x509_crt cert;
    mbedtls_pk_context key;
#endif
    bool initialised;
    bool connected;
} tls_session_t;

/* TCP connect + TLS handshake. Returns FW_ERR_UNSUPPORTED if built without mbedTLS. */
int tls_connect(tls_session_t *s, const char *host, uint16_t port, const tls_config_t *cfg);
/* Expose a connected session as a transport; transport close == tls_close. */
void tls_transport(tls_session_t *s, fw_transport_t *out);
void tls_close(tls_session_t *s);

#endif /* SECURITY_TLS_H */
