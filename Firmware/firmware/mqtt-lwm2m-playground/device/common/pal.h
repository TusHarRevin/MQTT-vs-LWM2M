#ifndef PAL_H
#define PAL_H

/*
 * Platform abstraction layer: the only file set that touches the OS.
 *
 * On a real board this is what gets replaced. The socket functions below map
 * directly onto a cellular modem's AT socket service (e.g. EC200U
 * AT+QIOPEN / AT+QISEND / AT+QIRD / AT+QICLOSE over an STM32 UART), so the
 * MQTT and LwM2M code above never changes.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- Time ---------------------------------------------------------------- */
uint64_t pal_now_us(void);      /* monotonic microseconds */
uint64_t pal_wall_ms(void);     /* Unix epoch milliseconds */
void pal_sleep_ms(uint32_t ms);
static inline double pal_ms_since(uint64_t start_us)
{
    return (double)(pal_now_us() - start_us) / 1000.0;
}

/* ---- Sockets ------------------------------------------------------------- */
typedef intptr_t pal_sock_t;
#define PAL_SOCK_INVALID ((pal_sock_t)-1)

#define PAL_ERR      (-1)
#define PAL_CLOSED   (-2)  /* TCP peer closed the connection */

int pal_net_init(void);

/* Resolve, create and connect. UDP sockets are connected too, so send/recv
 * only talk to that one peer. */
int pal_sock_connect(pal_sock_t *out, bool udp, const char *host, uint16_t port);
/* Returns bytes written or PAL_ERR. */
int pal_sock_send(pal_sock_t s, const void *data, size_t len);
/* Returns bytes read (> 0), 0 on timeout, PAL_CLOSED or PAL_ERR.
 * timeout_ms == 0 polls. */
int pal_sock_recv(pal_sock_t s, void *buf, size_t cap, uint32_t timeout_ms);
void pal_sock_close(pal_sock_t s);

/* Local UDP helpers used for the lab side-channel (events, control). */
int pal_udp_bind(pal_sock_t *out, const char *ip, uint16_t port);
int pal_udp_sendto(pal_sock_t s, const char *ip, uint16_t port, const void *data, size_t len);

/* TCP server helpers (lab server only). */
int pal_tcp_listen(pal_sock_t *out, const char *ip, uint16_t port);
pal_sock_t pal_tcp_accept(pal_sock_t listener);
/* Send everything; returns 0 or PAL_ERR. */
int pal_sock_send_all(pal_sock_t s, const void *data, size_t len);

/* ---- Deterministic PRNG (xorshift64*) ------------------------------------ */
typedef struct { uint64_t s; } pal_rng_t;
void pal_rng_seed(pal_rng_t *r, uint64_t seed);
uint32_t pal_rng_u32(pal_rng_t *r);
double pal_rng_unit(pal_rng_t *r);           /* [0, 1) */
double pal_rng_gauss(pal_rng_t *r);          /* mean 0, stddev 1 */

/* ---- Misc ---------------------------------------------------------------- */
/* "12:31:01.020" local time for a wall-clock ms timestamp. */
void pal_format_clock(uint64_t wall_ms, char out[16]);
bool pal_file_exists(const char *path);
int pal_mkdir(const char *path);

#endif /* PAL_H */
