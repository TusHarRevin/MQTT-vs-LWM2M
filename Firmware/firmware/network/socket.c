#include "network/socket.h"

#include <limits.h>
#include <string.h>

#include "common/fw_log.h"

#define TAG "sock"

int fw_socket_connect(fw_socket_t *s, port_sock_type_t type, const char *host, uint16_t port)
{
    int rc;

    if (s == NULL || host == NULL || port == 0) {
        return FW_ERR_PARAM;
    }
    memset(s, 0, sizeof(*s));
    s->handle = PORT_SOCK_INVALID;
    s->type = type;

    rc = port_sock_open(&s->handle, type, host, port);
    if (rc != FW_OK) {
        LOG_W(TAG, "%s %s:%u failed (%d)", type == PORT_SOCK_TCP ? "tcp" : "udp",
              host, (unsigned)port, rc);
        return rc;
    }
    s->open = true;
    LOG_D(TAG, "%s %s:%u connected", type == PORT_SOCK_TCP ? "tcp" : "udp",
          host, (unsigned)port);
    return FW_OK;
}

int fw_socket_send(fw_socket_t *s, const uint8_t *data, size_t len)
{
    size_t sent = 0;

    if (s == NULL || !s->open) {
        return FW_ERR_STATE;
    }
    if (len > INT_MAX) {
        return FW_ERR_PARAM;
    }
    while (sent < len) {
        int rc = port_sock_send(s->handle, data + sent, len - sent);
        if (rc < 0) {
            return rc;
        }
        if (rc == 0) {
            return FW_ERR_IO;
        }
        sent += (size_t)rc;
        if (s->type == PORT_SOCK_UDP) {
            break;
        }
    }
    s->tx_bytes += (uint32_t)sent;
    return (int)sent;
}

int fw_socket_recv(fw_socket_t *s, uint8_t *buf, size_t cap, uint32_t timeout_ms)
{
    int rc;

    if (s == NULL || !s->open) {
        return FW_ERR_STATE;
    }
    rc = port_sock_recv(s->handle, buf, cap, timeout_ms);
    if (rc > 0) {
        s->rx_bytes += (uint32_t)rc;
    }
    return rc;
}

void fw_socket_close(fw_socket_t *s)
{
    if (s != NULL && s->open) {
        port_sock_close(s->handle);
        s->open = false;
        s->handle = PORT_SOCK_INVALID;
    }
}
