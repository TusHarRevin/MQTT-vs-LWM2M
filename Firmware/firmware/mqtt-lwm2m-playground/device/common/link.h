#ifndef LINK_H
#define LINK_H

/*
 * Transport used by every protocol client:
 *
 *     protocol (MQTT / CoAP)
 *          |  link_send / link_recv
 *     [ TLS or DTLS (mbedTLS) ]        optional
 *          |
 *     [ netsim ]                        optional latency / loss
 *          |
 *     pal_sock_*                        OS socket today, modem AT socket later
 *
 * Byte counters are taken at the socket, i.e. what would go over the air,
 * including TLS/DTLS record overhead.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "netsim.h"
#include "pal.h"

typedef enum { LINK_TCP, LINK_UDP } link_kind_t;

typedef struct
{
    bool enabled;
    /* TLS (MQTT): verify the server against this CA (PEM file). */
    const char *ca_file;
    const char *server_name; /* checked against the certificate; default = host */
    bool tls12_only;         /* cap at TLS 1.2 (default lets TLS 1.3 negotiate) */
    /* DTLS (LwM2M): pre-shared key. */
    const char *psk_identity;
    const uint8_t *psk;
    size_t psk_len;
} link_security_t;

typedef struct
{
    uint64_t tx_bytes, rx_bytes;     /* on the wire */
    uint32_t tx_packets, rx_packets; /* send() calls / receive bursts */
    double connect_ms;               /* TCP connect (UDP: socket setup) */
    double handshake_ms;             /* TLS/DTLS handshake, 0 if plain */
    uint64_t hs_tx_bytes, hs_rx_bytes;
    char security[32];               /* "none", "TLSv1.3", "DTLSv1.2", ... */
    char cipher[64];
} link_stats_t;

/* Sizes can be lowered for a microcontroller build (-D...). */
#ifndef LINK_QUEUE_LEN
#define LINK_QUEUE_LEN 256     /* packets per netsim delay line */
#endif
#ifndef LINK_SCRATCH_SIZE
#define LINK_SCRATCH_SIZE 8192 /* receive buffer while netsim is active */
#endif

typedef struct
{
    link_kind_t kind;
    pal_sock_t sock;
    bool open;
    netsim_t *sim;
    link_stats_t st;
    /* netsim delay lines (only used while netsim is active) */
    struct link_q {
        struct link_pkt { uint8_t *data; size_t len, off; uint64_t due_us; } pkt[LINK_QUEUE_LEN];
        unsigned head, count;
        uint64_t last_due; /* TCP: keep the byte stream in order */
    } txq, rxq;
    bool peer_closed;
    uint8_t scratch[LINK_SCRATCH_SIZE];
    void *tls; /* mbedTLS state when secure */
    char err[128];
} link_t;

#define LINK_TIMEOUT 0
#define LINK_ERR (-1)
#define LINK_CLOSED (-2)

/* Opens the socket and, if sec->enabled, runs the TLS (TCP) or DTLS (UDP)
 * handshake. On failure link->err describes why. */
int link_open(link_t *l, link_kind_t kind, const char *host, uint16_t port,
              const link_security_t *sec, netsim_t *sim, uint32_t timeout_ms);
/* Sends the whole buffer. Returns len or LINK_ERR. */
int link_send(link_t *l, const uint8_t *data, size_t len);
/* Returns bytes (> 0), LINK_TIMEOUT, LINK_CLOSED or LINK_ERR.
 * UDP: one datagram per call. */
int link_recv(link_t *l, uint8_t *buf, size_t cap, uint32_t timeout_ms);
void link_close(link_t *l);

/* Reads a whole file into a NUL-terminated heap buffer (for PEM files). */
char *link_read_file(const char *path, size_t *len);
/* "00ff.." -> bytes; returns length or -1. */
int link_hex_decode(const char *hex, uint8_t *out, size_t cap);

#endif /* LINK_H */
