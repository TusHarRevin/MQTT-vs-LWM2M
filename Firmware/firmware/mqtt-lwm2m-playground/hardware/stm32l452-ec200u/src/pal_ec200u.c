/*
 * pal.h for the STM32L452 + EC200U: the board-side twin of device/common/pal.c.
 *
 *   time       TIM2 (1 MHz) for monotonic time; wall clock set by the lab
 *              ("time <epoch_ms> <utc_offset_min>" over the serial link)
 *   sockets    EC200U TCP/UDP sockets (AT+QIOPEN / QISEND / pushed data)
 *   lab link   events.c and control.c open "UDP sockets" on 127.0.0.1; here
 *              those become the ST-LINK serial port: events are written as
 *              one JSON object per line, commands are read one per line.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "board.h"
#include "ec200u.h"
#include "pal.h"
#include "pal_hw.h"

#define VCP_OUT ((pal_sock_t)1000) /* events.c's sender */
#define VCP_IN ((pal_sock_t)1001)  /* control.c's command channel */

static int64_t wall_offset_ms;
static int32_t utc_offset_min;

void pal_hw_set_wall(uint64_t epoch_ms, int32_t offset_min)
{
    wall_offset_ms = (int64_t)epoch_ms - (int64_t)(board_us() / 1000u);
    utc_offset_min = offset_min;
}

bool pal_hw_wall_known(void)
{
    return wall_offset_ms != 0;
}

/* ---- Time ---------------------------------------------------------------- */

uint64_t pal_now_us(void)
{
    return board_us();
}

uint64_t pal_wall_ms(void)
{
    return (uint64_t)((int64_t)(board_us() / 1000u) + wall_offset_ms);
}

void pal_sleep_ms(uint32_t ms)
{
    ec_pump(ms); /* keep servicing the modem while "sleeping" */
}

void pal_format_clock(uint64_t wall_ms, char out[16])
{
    time_t secs = (time_t)(wall_ms / 1000u) + (time_t)utc_offset_min * 60;
    struct tm tmv;
    gmtime_r(&secs, &tmv);
    snprintf(out, 16, "%02d:%02d:%02d.%03u", tmv.tm_hour, tmv.tm_min, tmv.tm_sec, (unsigned)(wall_ms % 1000u));
}

bool pal_file_exists(const char *path)
{
    (void)path;
    return false;
}

int pal_mkdir(const char *path)
{
    (void)path;
    return PAL_ERR;
}

/* ---- Sockets --------------------------------------------------------------- */

int pal_net_init(void)
{
    return ec_status()->pdp_up ? 0 : PAL_ERR;
}

int pal_sock_connect(pal_sock_t *out, bool udp, const char *host, uint16_t port)
{
    int id = ec_sock_open(udp, host, port, 60000);
    if (id < 0) {
        return PAL_ERR;
    }
    *out = (pal_sock_t)id;
    return 0;
}

int pal_sock_send(pal_sock_t s, const void *data, size_t len)
{
    return ec_sock_send((int)s, data, len) == (int)len ? (int)len : PAL_ERR;
}

int pal_sock_send_all(pal_sock_t s, const void *data, size_t len)
{
    return pal_sock_send(s, data, len) == (int)len ? 0 : PAL_ERR;
}

int pal_sock_recv(pal_sock_t s, void *buf, size_t cap, uint32_t timeout_ms)
{
    int n;

    if (s == VCP_IN) {
        uint32_t start = HAL_GetTick();
        do {
            if (vcp_read_line(buf, cap)) {
                if (strcmp(buf, "reset") == 0) {
                    board_reset(); /* the lab's way out of a stuck run */
                }
                return (int)strlen(buf);
            }
            ec_pump(0);
        } while (HAL_GetTick() - start < timeout_ms);
        return 0;
    }
    if (s == VCP_OUT) {
        return 0;
    }
    n = ec_sock_recv((int)s, buf, cap, timeout_ms);
    if (n == -2) {
        return PAL_CLOSED;
    }
    return n < 0 ? PAL_ERR : n;
}

void pal_sock_close(pal_sock_t s)
{
    if (s >= 0 && s < EC_MAX_SOCKETS) {
        ec_sock_close((int)s);
    }
}

int pal_udp_bind(pal_sock_t *out, const char *ip, uint16_t port)
{
    (void)ip;
    /* events.c binds port 0 to send, control.c binds its command port */
    *out = port == 0 ? VCP_OUT : VCP_IN;
    return 0;
}

int pal_udp_sendto(pal_sock_t s, const char *ip, uint16_t port, const void *data, size_t len)
{
    (void)ip;
    (void)port;
    if (s != VCP_OUT) {
        return PAL_ERR;
    }
    vcp_write(data, len);
    vcp_write("\r\n", 2);
    return 0;
}

int pal_tcp_listen(pal_sock_t *out, const char *ip, uint16_t port)
{
    (void)out;
    (void)ip;
    (void)port;
    return PAL_ERR;
}

pal_sock_t pal_tcp_accept(pal_sock_t listener)
{
    (void)listener;
    return PAL_SOCK_INVALID;
}

/* ---- PRNG (same as device/common/pal.c) -------------------------------------- */

void pal_rng_seed(pal_rng_t *r, uint64_t seed)
{
    r->s = seed ? seed : 0x9E3779B97F4A7C15ULL;
}

uint32_t pal_rng_u32(pal_rng_t *r)
{
    uint64_t x = r->s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    r->s = x;
    return (uint32_t)((x * 0x2545F4914F6CDD1DULL) >> 32);
}

double pal_rng_unit(pal_rng_t *r)
{
    return pal_rng_u32(r) / 4294967296.0;
}

double pal_rng_gauss(pal_rng_t *r)
{
    double u1 = pal_rng_unit(r), u2 = pal_rng_unit(r);
    if (u1 < 1e-12) {
        u1 = 1e-12;
    }
    return sqrt(-2.0 * log(u1)) * cos(6.283185307179586 * u2);
}
