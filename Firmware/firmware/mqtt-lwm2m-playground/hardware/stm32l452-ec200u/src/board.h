#ifndef BOARD_H
#define BOARD_H

/*
 * NUCLEO-L452RE-P board support: clocks, timers, the two UARTs and the RNG.
 *
 *   USART2 (PA2/PA3)   ST-LINK virtual COM port: console + lab link
 *   USART1 (PA9/PA10)  EC200U AT port
 *   TIM2               free-running 1 MHz counter (pal_now_us)
 *
 * Both UARTs are interrupt driven into ring buffers, so bytes keep arriving
 * while the code is busy elsewhere.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "stm32l4xx_hal.h"

void board_init(void);
void board_led(bool on);
void board_reset(void) __attribute__((noreturn));

/* Monotonic microseconds since boot (TIM2 + overflow count). */
uint64_t board_us(void);

/* Hardware RNG. Returns 0 on success. */
int board_rng(uint8_t *out, size_t len);

/* ---- ST-LINK virtual COM port --------------------------------------------- */
/* Queues bytes for transmission; blocks only while the 8 KiB queue is full. */
void vcp_write(const void *data, size_t len);
/* Waits until everything queued has left the UART. */
void vcp_flush(void);
/* Pops one received line (CR/LF stripped, NUL terminated). Non-blocking:
 * returns false if no complete line is waiting. */
bool vcp_read_line(char *out, size_t cap);

/* ---- EC200U UART ------------------------------------------------------------ */
void modem_uart_write(const void *data, size_t len);
/* Next received byte, or -1 if none. */
int modem_uart_getc(void);
void modem_uart_drop_rx(void);
/* Bytes lost because the ring buffer was full (should stay 0). */
uint32_t modem_uart_overruns(void);
/* Pulse PWRKEY (if wired, see hw_config.h). Returns false if not configured. */
bool modem_pwrkey_pulse(void);

#endif /* BOARD_H */
