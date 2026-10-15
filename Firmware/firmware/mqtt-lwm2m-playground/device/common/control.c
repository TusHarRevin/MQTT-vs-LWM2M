#include "control.h"

#include <string.h>

#include "pal.h"

static pal_sock_t s_sock = PAL_SOCK_INVALID;
static uint16_t s_port;

int ctl_open(uint16_t port)
{
    if (port == 0) {
        return 0;
    }
    if (pal_udp_bind(&s_sock, "127.0.0.1", port) != 0) {
        s_sock = PAL_SOCK_INVALID;
        return -1;
    }
    s_port = port;
    return 0;
}

uint16_t ctl_port(void)
{
    return s_sock == PAL_SOCK_INVALID ? 0 : s_port;
}

int ctl_poll(char *buf, size_t cap)
{
    int n;
    if (s_sock == PAL_SOCK_INVALID || cap < 2) {
        return 0;
    }
    n = pal_sock_recv(s_sock, buf, cap - 1, 0);
    if (n <= 0) {
        return 0;
    }
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' ')) {
        n--;
    }
    buf[n] = '\0';
    return n;
}

void ctl_close(void)
{
    pal_sock_close(s_sock);
    s_sock = PAL_SOCK_INVALID;
}
