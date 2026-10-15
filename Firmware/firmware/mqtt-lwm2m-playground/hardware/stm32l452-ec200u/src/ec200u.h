#ifndef EC200U_H
#define EC200U_H

/*
 * Quectel EC200U AT driver: network bring-up and TCP/UDP sockets.
 *
 * Sockets use the TCP/IP AT service in *direct push* mode
 * (AT+QIOPEN access_mode 1): received data arrives unsolicited as
 *     +QIURC: "recv",<id>,<len>\r\n<len raw bytes>
 * and is copied into a per-socket buffer straight away. No AT+QIRD round
 * trip per packet, so measured latencies are network + UART, nothing else.
 *
 * Everything runs in the main loop; ec_pump() must be called while waiting
 * (pal_sleep_ms / pal_sock_recv do that).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EC_MAX_SOCKETS 2

typedef struct
{
    bool sim_ready;
    bool registered;    /* +CEREG 1 or 5 */
    bool pdp_up;
    int rssi;           /* +CSQ, 99 = unknown */
    char op[40];        /* operator name */
    char ip[48];
    char imei[20];
    char rat[16];       /* "FDD LTE", "TDD LTE", "GSM" ... from AT+QNWINFO */
    char err[96];       /* why bring-up failed */
    uint32_t resets;    /* times the module rebooted by itself ("RDY") since our bring-up */
} ec_status_t;

/* Talks to the module (power key if silent), waits for SIM, LTE registration
 * and activates the PDP context with `apn`. progress() is called with short
 * human messages. Returns 0 when the data connection is up. */
int ec_bringup(const char *apn, void (*progress)(const char *msg));
/* Refresh signal/operator/IP fields. */
void ec_refresh_status(void);
const ec_status_t *ec_status(void);

/* Service the UART: parse lines, URCs and pushed socket data for up to
 * timeout_ms (0 = only what is already buffered). */
void ec_pump(uint32_t timeout_ms);

/* Print every AT command and modem line ("[at>]" / "[at<]") to the console. */
extern bool ec_trace;

/* Raw AT command for debugging; every response line goes to on_line. Returns
 * 0 on OK. */
int ec_at_passthrough(const char *cmd, uint32_t timeout_ms, void (*on_line)(const char *line));

/* ---- sockets (ids 0..EC_MAX_SOCKETS-1) ---- */
/* Opens a TCP connection or a connected UDP socket. Blocks until the module
 * reports +QIOPEN (TCP: handshake done). Returns the id or -1. */
int ec_sock_open(bool udp, const char *host, uint16_t port, uint32_t timeout_ms);
/* Returns len or -1. TCP payloads are split into 1460-byte AT+QISEND chunks. */
int ec_sock_send(int id, const void *data, size_t len);
/* Returns bytes (> 0), 0 on timeout, -2 if the peer closed, -1 on error.
 * UDP: one datagram per call (truncated to cap). */
int ec_sock_recv(int id, void *buf, size_t cap, uint32_t timeout_ms);
void ec_sock_close(int id);

#endif /* EC200U_H */
