#ifndef NETWORK_SOCKET_H
#define NETWORK_SOCKET_H

#include "platform/port.h"

typedef struct {
    port_sock_t handle;
    port_sock_type_t type;
    bool open;
    uint32_t tx_bytes;
    uint32_t rx_bytes;
} fw_socket_t;

int fw_socket_connect(fw_socket_t *s, port_sock_type_t type, const char *host, uint16_t port);
/* TCP: sends all bytes. UDP: sends one datagram. Returns len or < 0. */
int fw_socket_send(fw_socket_t *s, const uint8_t *data, size_t len);
/* Same return convention as port_sock_recv(). */
int fw_socket_recv(fw_socket_t *s, uint8_t *buf, size_t cap, uint32_t timeout_ms);
void fw_socket_close(fw_socket_t *s);

#endif /* NETWORK_SOCKET_H */
