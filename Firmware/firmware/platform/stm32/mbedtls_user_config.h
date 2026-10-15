/*
 * mbedTLS overlay for STM32 builds, applied on top of the default
 * mbedtls_config.h via MBEDTLS_USER_CONFIG_FILE (set in cmake/deps.cmake).
 */

/* No OS sockets, filesystem or POSIX timing: the firmware supplies its own. */
#undef MBEDTLS_NET_C
#undef MBEDTLS_TIMING_C
#undef MBEDTLS_FS_IO
#undef MBEDTLS_PSA_ITS_FILE_C
#undef MBEDTLS_PSA_CRYPTO_STORAGE_C

/* Entropy from the RNG peripheral: mbedtls_hardware_poll() in port_stm32.c. */
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT

/* No RTC is wired up yet, so there is no wall-clock time.
 * WARNING: certificate validity dates are NOT checked. Provide time via
 * MBEDTLS_PLATFORM_TIME_ALT and re-enable these once the RTC is synced. */
#undef MBEDTLS_HAVE_TIME
#undef MBEDTLS_HAVE_TIME_DATE

/* Client-only build: drop server-side features. */
#undef MBEDTLS_SSL_SRV_C
#undef MBEDTLS_SSL_CACHE_C
#undef MBEDTLS_SSL_TICKET_C
#undef MBEDTLS_SSL_COOKIE_C

/* TLS 1.2 / DTLS 1.2 only to save flash. Re-enable if your broker needs 1.3. */
#undef MBEDTLS_SSL_PROTO_TLS1_3

/* RAM: incoming records can be up to 16 KB unless the server honours the
 * max_fragment_length extension. Shrink only if your server supports it:
 * #define MBEDTLS_SSL_IN_CONTENT_LEN  4096
 * #define MBEDTLS_SSL_OUT_CONTENT_LEN 2048
 */
