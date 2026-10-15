#ifndef NETWORK_H
#define NETWORK_H

#include "network/socket.h"

/*
 * Byte-stream / datagram transport. Implemented by plain sockets (here), TLS
 * and DTLS (security/), so protocol clients don't care what's underneath.
 *
 * recv returns > 0 bytes, FW_ERR_TIMEOUT, FW_ERR_CLOSED or another error.
 */
typedef struct fw_transport {
    void *ctx;
    int (*send)(void *ctx, const uint8_t *data, size_t len);
    int (*recv)(void *ctx, uint8_t *buf, size_t cap, uint32_t timeout_ms);
    void (*close)(void *ctx);
} fw_transport_t;

static inline int fw_transport_send(const fw_transport_t *t, const uint8_t *data, size_t len)
{
    return t->send(t->ctx, data, len);
}

static inline int fw_transport_recv(const fw_transport_t *t, uint8_t *buf, size_t cap,
                                    uint32_t timeout_ms)
{
    return t->recv(t->ctx, buf, cap, timeout_ms);
}

static inline void fw_transport_close(fw_transport_t *t)
{
    if (t->close != NULL) {
        t->close(t->ctx);
    }
    t->ctx = NULL;
    t->send = NULL;
    t->recv = NULL;
    t->close = NULL;
}

static inline bool fw_transport_is_open(const fw_transport_t *t)
{
    return t->close != NULL;
}

int network_init(void);
bool network_is_up(void);

/* Wrap an already-connected socket as a transport. */
void network_socket_transport(fw_socket_t *sock, fw_transport_t *out);

#endif /* NETWORK_H */
