#ifndef UPSTREAM_H
#define UPSTREAM_H

/*
 * A TCP connection from the lab to a server's web API, plain or TLS
 * (the public Leshan sandbox only speaks HTTPS).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct upstream upstream_t;

/* Call once from main() before any thread uses upstream_open. */
void upstream_init(void);
/* tls: verify the server against ca_file (PEM) and host name. NULL on failure. */
upstream_t *upstream_open(const char *host, uint16_t port, bool tls, const char *ca_file);
/* 0 or -1. */
int upstream_send_all(upstream_t *u, const void *data, size_t len);
/* Bytes (> 0), 0 on timeout, -1 when closed or on error. */
int upstream_recv(upstream_t *u, void *buf, size_t cap, uint32_t timeout_ms);
void upstream_close(upstream_t *u);

#endif /* UPSTREAM_H */
