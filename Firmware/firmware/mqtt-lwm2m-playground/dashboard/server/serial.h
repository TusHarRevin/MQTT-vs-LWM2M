#ifndef SERIAL_H
#define SERIAL_H

/* Minimal serial port access for the lab's hardware link (ST-LINK VCP). */

#include <stddef.h>
#include <stdint.h>

typedef struct serial serial_t;

/* "COM10" (Windows) or "/dev/ttyACM0". 8N1, no flow control. NULL on error. */
serial_t *serial_open(const char *port, unsigned baud);
/* Returns bytes read (> 0), 0 on timeout, -1 if the port went away. */
int serial_read(serial_t *s, void *buf, size_t cap, uint32_t timeout_ms);
/* Writes everything; returns 0 or -1. */
int serial_write(serial_t *s, const void *data, size_t len);
void serial_close(serial_t *s);

#endif /* SERIAL_H */
