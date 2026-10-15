#ifndef HTTP_H
#define HTTP_H

/* Just enough HTTP/1.1 for a localhost dashboard. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pal.h"

typedef struct
{
    char method[8];
    char path[512];   /* without query string */
    char query[1024];
    char raw_head[8192]; /* request line + headers as received */
    size_t head_len;
    long content_length;
    bool keep_alive;
    long range_start, range_end; /* -1 if no Range header */
    char *body;       /* malloc'd, NUL terminated, may be NULL */
    /* bytes read past the headers that belong to the body */
    char extra[8192];
    size_t extra_len;
} http_req_t;

/* Returns 0 on success, -1 on EOF/error. */
int http_read_request(pal_sock_t s, http_req_t *r);
void http_free_request(http_req_t *r);
/* Value of a query parameter (URL-decoded) or NULL. */
const char *http_param(const http_req_t *r, const char *name, char *buf, size_t cap);
int http_respond(pal_sock_t s, int status, const char *ctype, const void *body, size_t len,
                 bool keep_alive);
int http_json(pal_sock_t s, int status, const char *json, bool keep_alive);
const char *http_mime(const char *path);

#endif /* HTTP_H */
