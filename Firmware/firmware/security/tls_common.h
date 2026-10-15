#ifndef SECURITY_TLS_COMMON_H
#define SECURITY_TLS_COMMON_H

/* Internal helpers shared by tls.c and dtls.c. */

#include "common/fw_types.h"

#if FW_HAVE_MBEDTLS
#include "mbedtls/ctr_drbg.h"

/* Idempotent: initialises PSA (if enabled) and seeds the shared DRBG. */
int fw_tls_rng_init(void);
mbedtls_ctr_drbg_context *fw_tls_rng(void);
void fw_tls_log_error(const char *tag, const char *what, int mbedtls_rc);
#endif

#endif /* SECURITY_TLS_COMMON_H */
