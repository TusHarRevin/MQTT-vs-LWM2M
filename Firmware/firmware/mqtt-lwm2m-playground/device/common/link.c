#include "link.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"

/* ---- Raw (socket + netsim delay lines) ----------------------------------- */

/* Without impairment, packets go straight to/from the socket. With it, each
 * one waits in a delay line (txq out, rxq in) until its due time; the queues
 * are serviced whenever the protocol sends or waits for data. */

static int q_push(struct link_q *q, const uint8_t *data, size_t len, uint64_t due)
{
    struct link_pkt *p;
    if (q->count == sizeof(q->pkt) / sizeof(q->pkt[0])) {
        return -1;
    }
    p = &q->pkt[(q->head + q->count) % (sizeof(q->pkt) / sizeof(q->pkt[0]))];
    p->data = malloc(len ? len : 1);
    if (p->data == NULL) {
        return -1;
    }
    memcpy(p->data, data, len);
    p->len = len;
    p->off = 0;
    p->due_us = due;
    q->count++;
    return 0;
}

static struct link_pkt *q_front(struct link_q *q)
{
    return q->count ? &q->pkt[q->head] : NULL;
}

static void q_pop(struct link_q *q)
{
    free(q->pkt[q->head].data);
    q->head = (q->head + 1) % (sizeof(q->pkt) / sizeof(q->pkt[0]));
    q->count--;
}

static void q_clear(struct link_q *q)
{
    while (q->count) {
        q_pop(q);
    }
    q->last_due = 0;
}

static int wire_send(link_t *l, const uint8_t *data, size_t len)
{
    if (l->kind == LINK_UDP) {
        return pal_sock_send(l->sock, data, len) < 0 ? LINK_ERR : (int)len;
    }
    return pal_sock_send_all(l->sock, data, len) < 0 ? LINK_ERR : (int)len;
}

/* Send every queued packet whose time has come. */
static int flush_tx(link_t *l)
{
    struct link_pkt *p;
    uint64_t now = pal_now_us();
    while ((p = q_front(&l->txq)) != NULL && p->due_us <= now) {
        int rc = wire_send(l, p->data, p->len);
        q_pop(&l->txq);
        if (rc < 0) {
            snprintf(l->err, sizeof(l->err), "socket send failed");
            return LINK_ERR;
        }
    }
    return 0;
}

static uint64_t due_after(link_t *l, struct link_q *q, uint32_t delay_ms)
{
    uint64_t due = pal_now_us() + (uint64_t)delay_ms * 1000ULL;
    if (l->kind == LINK_TCP && due < q->last_due) {
        due = q->last_due; /* a byte stream can't overtake itself */
    }
    q->last_due = due;
    return due;
}

static int raw_send(link_t *l, const uint8_t *data, size_t len)
{
    netsim_verdict_t v;

    l->st.tx_bytes += len;
    l->st.tx_packets++;
    if (!netsim_active(l->sim)) {
        return wire_send(l, data, len);
    }
    v = netsim_on_send(l->sim, l->kind == LINK_UDP);
    if (v.drop) {
        return (int)len; /* it left the device; the network ate it */
    }
    if (q_push(&l->txq, data, len, due_after(l, &l->txq, v.delay_ms)) != 0) {
        snprintf(l->err, sizeof(l->err), "netsim queue full");
        return LINK_ERR;
    }
    return flush_tx(l) < 0 ? LINK_ERR : (int)len;
}

static int raw_recv(link_t *l, uint8_t *buf, size_t cap, uint32_t timeout_ms)
{
    uint64_t deadline = pal_now_us() + (uint64_t)timeout_ms * 1000ULL;

    if (!netsim_active(l->sim)) {
        int n = pal_sock_recv(l->sock, buf, cap, timeout_ms);
        if (n <= 0) {
            return n == 0 ? LINK_TIMEOUT : (n == PAL_CLOSED ? LINK_CLOSED : LINK_ERR);
        }
        l->st.rx_bytes += (uint64_t)n;
        l->st.rx_packets++;
        return n;
    }

    for (;;) {
        struct link_pkt *p;
        uint64_t now, wake;
        int n;

        if (flush_tx(l) < 0) {
            return LINK_ERR;
        }
        now = pal_now_us();
        p = q_front(&l->rxq);
        if (p != NULL && p->due_us <= now) {
            size_t take = p->len - p->off;
            if (take > cap) {
                take = cap; /* TCP: the rest stays for the next read */
            }
            memcpy(buf, p->data + p->off, take);
            p->off += take;
            if (p->off == p->len || l->kind == LINK_UDP) {
                q_pop(&l->rxq);
            }
            return (int)take;
        }
        if (l->peer_closed && l->rxq.count == 0) {
            return LINK_CLOSED;
        }
        /* sleep in the socket until data arrives or a queued packet is due */
        wake = deadline;
        if (p != NULL && p->due_us < wake) wake = p->due_us;
        if (q_front(&l->txq) && q_front(&l->txq)->due_us < wake) wake = q_front(&l->txq)->due_us;
        if (wake < now) wake = now;
        if (l->peer_closed) {
            pal_sleep_ms((uint32_t)((wake - now + 999) / 1000));
            n = 0;
        } else {
            n = pal_sock_recv(l->sock, l->scratch, sizeof(l->scratch), (uint32_t)((wake - now + 999) / 1000));
        }
        if (n > 0) {
            netsim_verdict_t v = netsim_on_receive(l->sim, l->kind == LINK_UDP);
            if (!v.drop) {
                l->st.rx_bytes += (uint64_t)n;
                l->st.rx_packets++;
                if (q_push(&l->rxq, l->scratch, (size_t)n, due_after(l, &l->rxq, v.delay_ms)) != 0) {
                    return LINK_ERR;
                }
            }
            continue;
        }
        if (n == PAL_CLOSED) {
            l->peer_closed = true;
            continue;
        }
        if (n < 0) {
            return LINK_ERR;
        }
        if (pal_now_us() >= deadline && (q_front(&l->rxq) == NULL || q_front(&l->rxq)->due_us > deadline)) {
            return LINK_TIMEOUT;
        }
    }
}

/* ---- mbedTLS glue -------------------------------------------------------- */

typedef struct
{
    uint64_t start_us;
    uint32_t int_ms, fin_ms;
} dtls_timer_t;

typedef struct
{
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt ca;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    dtls_timer_t timer;
} tls_state_t;

static void timer_set(void *ctx, uint32_t int_ms, uint32_t fin_ms)
{
    dtls_timer_t *t = ctx;
    t->start_us = pal_now_us();
    t->int_ms = int_ms;
    t->fin_ms = fin_ms;
}

static int timer_get(void *ctx)
{
    dtls_timer_t *t = ctx;
    double el;
    if (t->fin_ms == 0) {
        return -1;
    }
    el = pal_ms_since(t->start_us);
    if (el >= t->fin_ms) {
        return 2;
    }
    return el >= t->int_ms ? 1 : 0;
}

static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    int rc = raw_send(ctx, buf, len);
    return rc < 0 ? MBEDTLS_ERR_NET_SEND_FAILED : rc;
}

static int bio_recv_timeout(void *ctx, unsigned char *buf, size_t len, uint32_t timeout)
{
    int rc = raw_recv(ctx, buf, len, timeout == 0 ? 3600000u : timeout);
    if (rc == LINK_TIMEOUT) {
        return MBEDTLS_ERR_SSL_TIMEOUT;
    }
    if (rc == LINK_CLOSED) {
        return MBEDTLS_ERR_NET_CONN_RESET;
    }
    return rc < 0 ? MBEDTLS_ERR_NET_RECV_FAILED : rc;
}

static void set_err(link_t *l, const char *what, int rc)
{
    char buf[96];
    mbedtls_strerror(rc, buf, sizeof(buf));
    snprintf(l->err, sizeof(l->err), "%s: %s (-0x%04x)", what, buf, (unsigned)-rc);
}

static void tls_free(tls_state_t *t)
{
    mbedtls_ssl_free(&t->ssl);
    mbedtls_ssl_config_free(&t->conf);
    mbedtls_x509_crt_free(&t->ca);
    mbedtls_ctr_drbg_free(&t->drbg);
    mbedtls_entropy_free(&t->entropy);
    free(t);
}

static int tls_start(link_t *l, const char *host, const link_security_t *sec, uint32_t timeout_ms)
{
    static const int dtls_psk_suites[] = {
        MBEDTLS_TLS_PSK_WITH_AES_128_CCM_8,
        MBEDTLS_TLS_PSK_WITH_AES_128_CBC_SHA256,
        0,
    };
    static bool psa_ready;
    const bool dtls = l->kind == LINK_UDP;
    tls_state_t *t = calloc(1, sizeof(*t));
    uint64_t hs_start;
    uint64_t tx0 = l->st.tx_bytes, rx0 = l->st.rx_bytes;
    int rc;

    if (t == NULL) {
        snprintf(l->err, sizeof(l->err), "out of memory");
        return LINK_ERR;
    }
    if (!psa_ready) {
        if (psa_crypto_init() != PSA_SUCCESS) {
            snprintf(l->err, sizeof(l->err), "psa_crypto_init failed");
            free(t);
            return LINK_ERR;
        }
        psa_ready = true;
    }
    mbedtls_ssl_init(&t->ssl);
    mbedtls_ssl_config_init(&t->conf);
    mbedtls_x509_crt_init(&t->ca);
    mbedtls_entropy_init(&t->entropy);
    mbedtls_ctr_drbg_init(&t->drbg);

    rc = mbedtls_ctr_drbg_seed(&t->drbg, mbedtls_entropy_func, &t->entropy,
                               (const unsigned char *)"pg-link", 7);
    if (rc == 0) {
        rc = mbedtls_ssl_config_defaults(&t->conf, MBEDTLS_SSL_IS_CLIENT,
                                         dtls ? MBEDTLS_SSL_TRANSPORT_DATAGRAM
                                              : MBEDTLS_SSL_TRANSPORT_STREAM,
                                         MBEDTLS_SSL_PRESET_DEFAULT);
    }
    if (rc != 0) {
        set_err(l, "TLS config", rc);
        tls_free(t);
        return LINK_ERR;
    }
    mbedtls_ssl_conf_rng(&t->conf, mbedtls_ctr_drbg_random, &t->drbg);
    mbedtls_ssl_conf_read_timeout(&t->conf, timeout_ms);

    if (dtls) {
        if (sec->psk == NULL || sec->psk_identity == NULL) {
            snprintf(l->err, sizeof(l->err), "DTLS needs a PSK identity and key");
            tls_free(t);
            return LINK_ERR;
        }
        mbedtls_ssl_conf_max_tls_version(&t->conf, MBEDTLS_SSL_VERSION_TLS1_2);
        mbedtls_ssl_conf_ciphersuites(&t->conf, dtls_psk_suites);
        rc = mbedtls_ssl_conf_psk(&t->conf, sec->psk, sec->psk_len,
                                  (const unsigned char *)sec->psk_identity,
                                  strlen(sec->psk_identity));
        if (rc != 0) {
            set_err(l, "DTLS PSK", rc);
            tls_free(t);
            return LINK_ERR;
        }
        /* RFC 6347 defaults: 1 s initial retransmission, doubling. */
        mbedtls_ssl_conf_handshake_timeout(&t->conf, 1000, timeout_ms < 2000 ? 2000 : timeout_ms);
    } else {
        if (sec->tls12_only) {
            mbedtls_ssl_conf_max_tls_version(&t->conf, MBEDTLS_SSL_VERSION_TLS1_2);
        }
        if (sec->ca_file != NULL) {
            /* Read it ourselves rather than mbedtls_x509_crt_parse_file, so
             * mbedTLS doesn't need MBEDTLS_FS_IO (absent on the STM32 build). */
            size_t pem_len = 0;
            char *pem = link_read_file(sec->ca_file, &pem_len);
            rc = pem ? mbedtls_x509_crt_parse(&t->ca, (const unsigned char *)pem, pem_len + 1)
                     : MBEDTLS_ERR_X509_FILE_IO_ERROR;
            free(pem);
            if (rc != 0) {
                set_err(l, "loading CA file", rc);
                tls_free(t);
                return LINK_ERR;
            }
            mbedtls_ssl_conf_ca_chain(&t->conf, &t->ca, NULL);
            mbedtls_ssl_conf_authmode(&t->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
        } else {
            /* No CA: encrypted but the broker is not authenticated. */
            mbedtls_ssl_conf_authmode(&t->conf, MBEDTLS_SSL_VERIFY_NONE);
        }
    }

    rc = mbedtls_ssl_setup(&t->ssl, &t->conf);
    if (rc == 0 && !dtls) {
        rc = mbedtls_ssl_set_hostname(&t->ssl, sec->server_name ? sec->server_name : host);
    }
    if (rc != 0) {
        set_err(l, "TLS setup", rc);
        tls_free(t);
        return LINK_ERR;
    }
    mbedtls_ssl_set_bio(&t->ssl, l, bio_send, NULL, bio_recv_timeout);
    if (dtls) {
        mbedtls_ssl_set_timer_cb(&t->ssl, &t->timer, timer_set, timer_get);
    }

    hs_start = pal_now_us();
    do {
        rc = mbedtls_ssl_handshake(&t->ssl);
    } while (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE);
    if (rc != 0) {
        uint32_t flags = mbedtls_ssl_get_verify_result(&t->ssl);
        set_err(l, dtls ? "DTLS handshake" : "TLS handshake", rc);
        if (flags != 0 && flags != (uint32_t)-1) {
            char vb[128];
            mbedtls_x509_crt_verify_info(vb, sizeof(vb), "", flags);
            vb[strcspn(vb, "\n")] = '\0';
            snprintf(l->err + strlen(l->err), sizeof(l->err) - strlen(l->err), " [%s]", vb);
        }
        tls_free(t);
        return LINK_ERR;
    }
    l->st.handshake_ms = pal_ms_since(hs_start);
    l->st.hs_tx_bytes = l->st.tx_bytes - tx0;
    l->st.hs_rx_bytes = l->st.rx_bytes - rx0;
    snprintf(l->st.security, sizeof(l->st.security), "%s", mbedtls_ssl_get_version(&t->ssl));
    snprintf(l->st.cipher, sizeof(l->st.cipher), "%s", mbedtls_ssl_get_ciphersuite(&t->ssl));
    l->tls = t;
    return 0;
}

/* ---- Public API ---------------------------------------------------------- */

int link_open(link_t *l, link_kind_t kind, const char *host, uint16_t port,
              const link_security_t *sec, netsim_t *sim, uint32_t timeout_ms)
{
    uint64_t t0;

    memset(l, 0, sizeof(*l));
    l->kind = kind;
    l->sim = sim;
    l->sock = PAL_SOCK_INVALID;
    snprintf(l->st.security, sizeof(l->st.security), "none");

    t0 = pal_now_us();
    if (pal_sock_connect(&l->sock, kind == LINK_UDP, host, port) != 0) {
        snprintf(l->err, sizeof(l->err), "cannot connect to %s:%u (%s) - is the server running?",
                 host, (unsigned)port, kind == LINK_UDP ? "udp" : "tcp");
        return LINK_ERR;
    }
    l->st.connect_ms = pal_ms_since(t0);
    l->open = true;

    if (sec != NULL && sec->enabled) {
        if (tls_start(l, host, sec, timeout_ms) != 0) {
            pal_sock_close(l->sock);
            l->open = false;
            return LINK_ERR;
        }
    }
    return 0;
}

int link_send(link_t *l, const uint8_t *data, size_t len)
{
    if (!l->open) {
        return LINK_ERR;
    }
    if (l->tls != NULL) {
        tls_state_t *t = l->tls;
        size_t done = 0;
        while (done < len) {
            int rc = mbedtls_ssl_write(&t->ssl, data + done, len - done);
            if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
                continue;
            }
            if (rc < 0) {
                set_err(l, "TLS write", rc);
                return LINK_ERR;
            }
            done += (size_t)rc;
        }
        return (int)len;
    }
    return raw_send(l, data, len);
}

int link_recv(link_t *l, uint8_t *buf, size_t cap, uint32_t timeout_ms)
{
    if (!l->open) {
        return LINK_ERR;
    }
    if (l->tls != NULL) {
        tls_state_t *t = l->tls;
        uint64_t start = pal_now_us();
        for (;;) {
            double spent = pal_ms_since(start);
            uint32_t left = spent >= timeout_ms ? 1 : timeout_ms - (uint32_t)spent;
            int rc;
            /* 0 would mean "block forever" to mbedTLS. */
            mbedtls_ssl_conf_read_timeout(&t->conf, left ? left : 1);
            rc = mbedtls_ssl_read(&t->ssl, buf, cap);
            if (rc > 0) {
                return rc;
            }
            if (rc == MBEDTLS_ERR_SSL_TIMEOUT) {
                return LINK_TIMEOUT;
            }
            if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE ||
                rc == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) {
                if (pal_ms_since(start) >= timeout_ms) {
                    return LINK_TIMEOUT;
                }
                continue;
            }
            if (rc == 0 || rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || rc == MBEDTLS_ERR_NET_CONN_RESET) {
                snprintf(l->err, sizeof(l->err), "%s",
                         rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY ? "server closed the TLS session (close_notify)"
                         : rc == 0 ? "server closed the connection" : "connection reset");
                return LINK_CLOSED;
            }
            set_err(l, "TLS read", rc);
            return LINK_ERR;
        }
    }
    return raw_recv(l, buf, cap, timeout_ms);
}

void link_close(link_t *l)
{
    /* Deliver what is still in the outgoing delay line, at its due time. */
    while (l->open && q_front(&l->txq) != NULL) {
        uint64_t now = pal_now_us(), due = q_front(&l->txq)->due_us;
        if (due > now) {
            pal_sleep_ms((uint32_t)((due - now + 999) / 1000));
        }
        if (flush_tx(l) < 0) {
            break;
        }
    }
    if (l->tls != NULL) {
        tls_state_t *t = l->tls;
        (void)mbedtls_ssl_close_notify(&t->ssl);
        tls_free(t);
        l->tls = NULL;
    }
    if (l->open) {
        pal_sock_close(l->sock);
        l->open = false;
    }
    q_clear(&l->txq);
    q_clear(&l->rxq);
}

char *link_read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    char *buf;
    long n;

    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)n + 1);
    if (buf != NULL) {
        if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
            free(buf);
            buf = NULL;
        } else {
            buf[n] = '\0';
            if (len) {
                *len = (size_t)n;
            }
        }
    }
    fclose(f);
    return buf;
}

int link_hex_decode(const char *hex, uint8_t *out, size_t cap)
{
    size_t n = strlen(hex), i;
    if (n % 2 != 0 || n / 2 > cap) {
        return -1;
    }
    for (i = 0; i < n / 2; i++) {
        unsigned v;
        if (sscanf(hex + 2 * i, "%2x", &v) != 1) {
            return -1;
        }
        out[i] = (uint8_t)v;
    }
    return (int)(n / 2);
}
