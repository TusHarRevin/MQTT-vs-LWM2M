/*
 * mbedTLS 3.6 configuration for the STM32L452 (512 KiB flash, 160 KiB RAM).
 *
 * Only what the playground uses:
 *   MQTT   TLS 1.3 / 1.2 client, server verified against the lab RSA CA
 *          (ECDHE key exchange on X25519 / P-256, AES-GCM)
 *   LwM2M  DTLS 1.2 client with a pre-shared key (AES-128-CCM-8 / CBC)
 *
 * No file system, no sockets (the EC200U is reached through link.c's own
 * callbacks), no OS time: certificate validity dates are NOT checked.
 */
#ifndef MBEDTLS_CONFIG_STM32_H
#define MBEDTLS_CONFIG_STM32_H

/* ---- platform ---- */
#define MBEDTLS_HAVE_ASM
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT   /* mbedtls_hardware_poll() -> STM32 RNG */
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_ERROR_C

/* ---- big numbers / public key ---- */
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_MPI_MAX_SIZE 512       /* RSA up to 4096 bits */
#define MBEDTLS_MPI_WINDOW_SIZE 2
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21              /* RSA-PSS: TLS 1.3 CertificateVerify */
#define MBEDTLS_X509_RSASSA_PSS_SUPPORT
#define MBEDTLS_ECP_C
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C                /* servers may sign with ECDSA */
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED
#define MBEDTLS_ECP_NIST_OPTIM
#define MBEDTLS_ECP_WINDOW_SIZE 2
#define MBEDTLS_ECP_FIXED_POINT_OPTIM 0
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_OID_C
#define MBEDTLS_BASE64_C
#define MBEDTLS_PEM_PARSE_C
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C

/* ---- symmetric ---- */
#define MBEDTLS_AES_C
#define MBEDTLS_AES_ROM_TABLES         /* tables in flash, not RAM */
#define MBEDTLS_AES_FEWER_TABLES
#define MBEDTLS_GCM_C
#define MBEDTLS_CCM_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_CIPHER_MODE_CBC
#define MBEDTLS_MD_C
#define MBEDTLS_SHA224_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C
#define MBEDTLS_SHA256_SMALLER
#define MBEDTLS_SHA512_SMALLER
#define MBEDTLS_HKDF_C

/* ---- PSA (required by TLS 1.3) ---- */
#define MBEDTLS_PSA_CRYPTO_C
#define MBEDTLS_PSA_KEY_SLOT_COUNT 8

/* ---- TLS / DTLS client ---- */
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_PROTO_TLS1_3
#define MBEDTLS_SSL_TLS1_3_COMPATIBILITY_MODE
#define MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_EPHEMERAL_ENABLED
#define MBEDTLS_SSL_KEEP_PEER_CERTIFICATE /* required by TLS 1.3 */
#define MBEDTLS_SSL_SESSION_TICKETS    /* accept the server's NewSessionTicket */
#define MBEDTLS_SSL_PROTO_DTLS
#define MBEDTLS_SSL_DTLS_HELLO_VERIFY  /* Leshan answers with a cookie */
#define MBEDTLS_SSL_DTLS_ANTI_REPLAY
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_SSL_ENCRYPT_THEN_MAC
#define MBEDTLS_SSL_EXTENDED_MASTER_SECRET
#define MBEDTLS_KEY_EXCHANGE_PSK_ENABLED          /* DTLS-PSK (LwM2M) */
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED    /* TLS 1.2 with the lab RSA cert */
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED

/* Record buffers: the lab's handshakes and MQTT packets are small. */
#define MBEDTLS_SSL_IN_CONTENT_LEN 6144
#define MBEDTLS_SSL_OUT_CONTENT_LEN 4096
#define MBEDTLS_SSL_DTLS_MAX_BUFFERING 4096

#endif /* MBEDTLS_CONFIG_STM32_H */
