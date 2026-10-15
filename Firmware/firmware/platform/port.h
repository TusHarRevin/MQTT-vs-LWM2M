#ifndef PORT_H
#define PORT_H

/*
 * Platform port contract.
 *
 * This is the only interface the portable firmware uses to reach hardware or
 * the OS. Each directory under platform/ provides one implementation:
 *   platform/pc/     - Windows / Linux / macOS simulation
 *   platform/stm32/  - STM32 HAL + LwIP
 *
 * Nothing above the hal/ and network/ layers may call port_* directly
 * (port_log via fw_log.h is the one exception).
 */

#include "common/fw_types.h"

/* ---- Lifecycle --------------------------------------------------------- */
int port_init(void);

/* ---- Time -------------------------------------------------------------- */
uint32_t port_millis(void);        /* monotonic, wraps at 2^32 */
void port_delay_ms(uint32_t ms);   /* busy or blocking delay */

/* ---- Power ------------------------------------------------------------- */
void port_sleep_ms(uint32_t ms);   /* low-power wait; may wake early */
void port_reset(void);             /* does not return */
int port_battery_mv(void);         /* < 0 if not available */

/* ---- Storage: small key/blob store ------------------------------------- */
int port_storage_read(const char *key, void *buf, size_t cap, size_t *out_len);
int port_storage_write(const char *key, const void *data, size_t len);
int port_storage_erase(const char *key);

/* ---- Network ----------------------------------------------------------- */
typedef enum { PORT_SOCK_TCP, PORT_SOCK_UDP } port_sock_type_t;
typedef intptr_t port_sock_t;
#define PORT_SOCK_INVALID ((port_sock_t)-1)

int port_net_init(void);
bool port_net_is_up(void);
/* Resolve host, create the socket and connect it (UDP sockets are connected
 * so plain send/recv work). */
int port_sock_open(port_sock_t *out, port_sock_type_t type, const char *host, uint16_t port);
/* Returns bytes sent (may be < len for TCP) or a negative fw_err_t. */
int port_sock_send(port_sock_t s, const uint8_t *data, size_t len);
/* Returns bytes received (> 0), FW_ERR_TIMEOUT, FW_ERR_CLOSED or another
 * negative fw_err_t. timeout_ms may be 0 (poll) or FW_WAIT_FOREVER. */
int port_sock_recv(port_sock_t s, uint8_t *buf, size_t cap, uint32_t timeout_ms);
void port_sock_close(port_sock_t s);

/* ---- Sensors ----------------------------------------------------------- */
int port_sensor_read(unsigned channel, float *value);

/* ---- Entropy (true random source, used for IDs) ------------------------ */
int port_entropy(uint8_t *buf, size_t len);

/* ---- Logging ----------------------------------------------------------- */
void port_log(int level, const char *tag, const char *fmt, ...);

#endif /* PORT_H */
