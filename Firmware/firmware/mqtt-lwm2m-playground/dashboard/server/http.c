#include "http.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int ci_prefix(const char *s, const char *prefix)
{
    for (; *prefix; s++, prefix++) {
        if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix)) {
            return 0;
        }
    }
    return 1;
}

int http_read_request(pal_sock_t s, http_req_t *r)
{
    size_t got = 0;
    char *end = NULL, *line, *q;
    memset(r, 0, sizeof(*r));
    r->range_start = r->range_end = -1;

    while (end == NULL) {
        int n;
        if (got >= sizeof(r->raw_head) - 1) {
            return -1;
        }
        n = pal_sock_recv(s, r->raw_head + got, sizeof(r->raw_head) - 1 - got, 30000);
        if (n <= 0) {
            return -1;
        }
        got += (size_t)n;
        r->raw_head[got] = '\0';
        end = strstr(r->raw_head, "\r\n\r\n");
    }
    r->head_len = (size_t)(end - r->raw_head) + 4;
    r->extra_len = got - r->head_len;
    memcpy(r->extra, r->raw_head + r->head_len, r->extra_len);

    /* request line */
    if (sscanf(r->raw_head, "%7s %511s", r->method, r->path) != 2) {
        return -1;
    }
    q = strchr(r->path, '?');
    if (q) {
        snprintf(r->query, sizeof(r->query), "%s", q + 1);
        *q = '\0';
    }
    r->keep_alive = strstr(r->raw_head, "HTTP/1.1") != NULL;

    /* headers */
    line = strstr(r->raw_head, "\r\n");
    while (line && line + 2 < r->raw_head + r->head_len - 2) {
        line += 2;
        if (ci_prefix(line, "content-length:")) {
            r->content_length = atol(line + 15);
        } else if (ci_prefix(line, "connection:")) {
            const char *v = line + 11;
            while (*v == ' ') v++;
            if (ci_prefix(v, "close")) r->keep_alive = false;
            if (ci_prefix(v, "keep-alive")) r->keep_alive = true;
        } else if (ci_prefix(line, "range:")) {
            const char *v = strstr(line, "bytes=");
            if (v) {
                char *dash;
                r->range_start = strtol(v + 6, &dash, 10);
                r->range_end = (*dash == '-' && isdigit((unsigned char)dash[1])) ? strtol(dash + 1, NULL, 10) : -1;
            }
        }
        line = strstr(line, "\r\n");
    }

    if (r->content_length > 0) {
        size_t have;
        if (r->content_length > 1024 * 1024) {
            return -1;
        }
        r->body = malloc((size_t)r->content_length + 1);
        if (r->body == NULL) {
            return -1;
        }
        have = r->extra_len < (size_t)r->content_length ? r->extra_len : (size_t)r->content_length;
        memcpy(r->body, r->extra, have);
        while (have < (size_t)r->content_length) {
            int n = pal_sock_recv(s, r->body + have, (size_t)r->content_length - have, 30000);
            if (n <= 0) {
                return -1;
            }
            have += (size_t)n;
        }
        r->body[r->content_length] = '\0';
    }
    return 0;
}

void http_free_request(http_req_t *r)
{
    free(r->body);
    r->body = NULL;
}

static int hexv(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = tolower(c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

const char *http_param(const http_req_t *r, const char *name, char *buf, size_t cap)
{
    size_t nl = strlen(name);
    const char *p = r->query;
    while (p && *p) {
        if (strncmp(p, name, nl) == 0 && p[nl] == '=') {
            const char *v = p + nl + 1;
            size_t o = 0;
            while (*v && *v != '&' && o + 1 < cap) {
                if (*v == '+') {
                    buf[o++] = ' ';
                    v++;
                } else if (*v == '%' && hexv(v[1]) >= 0 && hexv(v[2]) >= 0) {
                    buf[o++] = (char)(hexv(v[1]) * 16 + hexv(v[2]));
                    v += 3;
                } else {
                    buf[o++] = *v++;
                }
            }
            buf[o] = '\0';
            return buf;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
    return NULL;
}

static const char *reason(int status)
{
    switch (status) {
    case 200: return "OK";
    case 204: return "No Content";
    case 206: return "Partial Content";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 409: return "Conflict";
    case 416: return "Range Not Satisfiable";
    case 500: return "Internal Server Error";
    case 502: return "Bad Gateway";
    default: return "Status";
    }
}

int http_respond(pal_sock_t s, int status, const char *ctype, const void *body, size_t len,
                 bool keep_alive)
{
    char head[512];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %u\r\n"
                     "Cache-Control: no-store\r\nConnection: %s\r\n\r\n",
                     status, reason(status), ctype, (unsigned)len, keep_alive ? "keep-alive" : "close");
    if (pal_sock_send_all(s, head, (size_t)n) != 0) {
        return -1;
    }
    return len ? pal_sock_send_all(s, body, len) : 0;
}

int http_json(pal_sock_t s, int status, const char *json, bool keep_alive)
{
    return http_respond(s, status, "application/json", json, strlen(json), keep_alive);
}

const char *http_mime(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (dot == NULL) return "application/octet-stream";
    if (strcmp(dot, ".html") == 0) return "text/html; charset=utf-8";
    if (strcmp(dot, ".js") == 0) return "text/javascript; charset=utf-8";
    if (strcmp(dot, ".css") == 0) return "text/css; charset=utf-8";
    if (strcmp(dot, ".json") == 0) return "application/json";
    if (strcmp(dot, ".svg") == 0) return "image/svg+xml";
    if (strcmp(dot, ".png") == 0) return "image/png";
    if (strcmp(dot, ".ico") == 0) return "image/x-icon";
    if (strcmp(dot, ".md") == 0) return "text/plain; charset=utf-8";
    return "application/octet-stream";
}
