#ifndef CONTROL_H
#define CONTROL_H

/*
 * Tiny command channel so the dashboard can poke a running device
 * ("stop", "notify", "deregister", ...). It is a UDP socket on 127.0.0.1;
 * each datagram is one text command. Port 0 disables it.
 *
 * This is lab plumbing, not part of either protocol. On hardware the same
 * commands could arrive over a debug UART.
 */

#include <stddef.h>
#include <stdint.h>

int ctl_open(uint16_t port);
/* Non-blocking. Returns the command length (> 0) and copies it, NUL
 * terminated and trimmed, into buf; 0 if nothing is pending. */
int ctl_poll(char *buf, size_t cap);
uint16_t ctl_port(void);
void ctl_close(void);

#endif /* CONTROL_H */
