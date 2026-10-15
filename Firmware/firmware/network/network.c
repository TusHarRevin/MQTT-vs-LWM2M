#include "network/network.h"

#include "common/fw_log.h"

#define TAG "net"

int network_init(void)
{
    int rc = port_net_init();

    if (rc != FW_OK) {
        LOG_W(TAG, "link init failed (%d)", rc);
        return rc;
    }
    if (!port_net_is_up()) {
        return FW_ERR_STATE;
    }
    LOG_I(TAG, "network up");
    return FW_OK;
}

bool network_is_up(void)
{
    return port_net_is_up();
}

static int sock_send(void *ctx, const uint8_t *data, size_t len)
{
    return fw_socket_send((fw_socket_t *)ctx, data, len);
}

static int sock_recv(void *ctx, uint8_t *buf, size_t cap, uint32_t timeout_ms)
{
    return fw_socket_recv((fw_socket_t *)ctx, buf, cap, timeout_ms);
}

static void sock_close(void *ctx)
{
    fw_socket_close((fw_socket_t *)ctx);
}

void network_socket_transport(fw_socket_t *sock, fw_transport_t *out)
{
    out->ctx = sock;
    out->send = sock_send;
    out->recv = sock_recv;
    out->close = sock_close;
}
