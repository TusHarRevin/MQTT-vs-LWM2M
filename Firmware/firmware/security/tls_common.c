#include "security/tls_common.h"

#if FW_HAVE_MBEDTLS

#include "mbedtls/build_info.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#if defined(MBEDTLS_PSA_CRYPTO_C)
#include "psa/crypto.h"
#endif

#include "common/fw_log.h"

#define TAG "crypto"

/* Entropy comes from mbedTLS' configured sources: the OS on PC, and
 * mbedtls_hardware_poll() (the RNG peripheral) on STM32 - see
 * platform/stm32/mbedtls_user_config.h. */
static mbedtls_entropy_context s_entropy;
static mbedtls_ctr_drbg_context s_drbg;
static bool s_ready;

int fw_tls_rng_init(void)
{
    static const unsigned char pers[] = "fw-tls";
    int rc;

    if (s_ready) {
        return FW_OK;
    }
#if defined(MBEDTLS_PSA_CRYPTO_C)
    if (psa_crypto_init() != PSA_SUCCESS) {
        LOG_E(TAG, "psa_crypto_init failed");
        return FW_ERR_SECURITY;
    }
#endif
    mbedtls_entropy_init(&s_entropy);
    mbedtls_ctr_drbg_init(&s_drbg);
    rc = mbedtls_ctr_drbg_seed(&s_drbg, mbedtls_entropy_func, &s_entropy, pers, sizeof(pers) - 1);
    if (rc != 0) {
        fw_tls_log_error(TAG, "drbg seed", rc);
        mbedtls_ctr_drbg_free(&s_drbg);
        mbedtls_entropy_free(&s_entropy);
        return FW_ERR_SECURITY;
    }
    s_ready = true;
    return FW_OK;
}

mbedtls_ctr_drbg_context *fw_tls_rng(void)
{
    return s_ready ? &s_drbg : NULL;
}

void fw_tls_log_error(const char *tag, const char *what, int mbedtls_rc)
{
#if defined(MBEDTLS_ERROR_C)
    char buf[96];
    mbedtls_strerror(mbedtls_rc, buf, sizeof(buf));
    LOG_E(tag, "%s: -0x%04x %s", what, (unsigned)-mbedtls_rc, buf);
#else
    LOG_E(tag, "%s: -0x%04x", what, (unsigned)-mbedtls_rc);
#endif
}

#endif /* FW_HAVE_MBEDTLS */
