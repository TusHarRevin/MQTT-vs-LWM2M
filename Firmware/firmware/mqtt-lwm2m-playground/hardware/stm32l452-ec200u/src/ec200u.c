#include "ec200u.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "hw_config.h"

#define LINE_MAX 300u
#define SOCK_RX_SIZE 4096u /* per socket; board.c's 8 KiB UART ring is the backstop */
#define QISEND_MAX 1460u

enum { S_FREE, S_OPENING, S_OPEN, S_PEER_CLOSED };

typedef struct
{
    int state;
    bool udp;
    int open_err;
    uint32_t dropped;               /* bytes that didn't fit */
    uint8_t rx[SOCK_RX_SIZE];       /* TCP: byte stream; UDP: [len16][datagram]... */
    uint32_t head, tail;
} sock_t;

static struct {
    char line[LINE_MAX];
    size_t len;

    /* the command in progress */
    bool cmd_active;
    const char *final_ok;
    const char *prefix;
    char info[160];
    int final; /* 0 pending, 1 ok, -1 error */
    bool want_prompt, prompt;
    void (*echo)(const char *line);

    sock_t s[EC_MAX_SOCKETS];
    ec_status_t st;
    bool rebooted;   /* saw "RDY": our settings are gone, redo them */
    bool booted;     /* bring-up finished once */
    char apn[64];
} E;

/* ---- socket buffers ------------------------------------------------------- */

static uint32_t rb_used(const sock_t *s)
{
    return (s->head + SOCK_RX_SIZE - s->tail) % SOCK_RX_SIZE;
}

static uint32_t rb_free(const sock_t *s)
{
    return SOCK_RX_SIZE - 1u - rb_used(s);
}

static void rb_put(sock_t *s, uint8_t c)
{
    s->rx[s->head] = c;
    s->head = (s->head + 1u) % SOCK_RX_SIZE;
}

static uint8_t rb_get(sock_t *s)
{
    uint8_t c = s->rx[s->tail];
    s->tail = (s->tail + 1u) % SOCK_RX_SIZE;
    return c;
}

/* Blocking read of pushed data that follows a +QIURC "recv" line. */
static int raw_getc(uint32_t timeout_ms)
{
    uint32_t start = HAL_GetTick();
    int c;
    while ((c = modem_uart_getc()) < 0) {
        if (HAL_GetTick() - start > timeout_ms) {
            return -1;
        }
    }
    return c;
}

static void on_recv(int id, int len)
{
    sock_t *s = (id >= 0 && id < EC_MAX_SOCKETS) ? &E.s[id] : NULL;
    bool keep = s != NULL && (uint32_t)len + (s->udp ? 2u : 0u) <= rb_free(s);

    if (keep && s->udp) {
        rb_put(s, (uint8_t)(len >> 8));
        rb_put(s, (uint8_t)len);
    }
    for (int i = 0; i < len; i++) {
        int c = raw_getc(1000);
        if (c < 0) {
            break; /* the module stopped mid-packet; nothing sensible to do */
        }
        if (keep) {
            rb_put(s, (uint8_t)c);
        }
    }
    if (!keep && s != NULL) {
        s->dropped += (uint32_t)len;
        printf("[ec200u] socket %d: buffer full, dropped %d bytes\n", id, len);
    }
}

/* ---- line handling ---------------------------------------------------------- */

static bool starts(const char *s, const char *p)
{
    return strncmp(s, p, strlen(p)) == 0;
}

bool ec_trace;

static void on_line(const char *l)
{
    int id, v;

    if (ec_trace) {
        printf("[at<] %s\n", l);
    }
    if (sscanf(l, "+QIURC: \"recv\",%d,%d", &id, &v) == 2) {
        on_recv(id, v);
        return;
    }
    if (sscanf(l, "+QIURC: \"closed\",%d", &id) == 1) {
        if (id >= 0 && id < EC_MAX_SOCKETS && E.s[id].state != S_FREE) {
            E.s[id].state = S_PEER_CLOSED;
        }
        return;
    }
    if (starts(l, "+QIURC: \"pdpdeact\"")) {
        E.st.pdp_up = false;
        for (int i = 0; i < EC_MAX_SOCKETS; i++) {
            if (E.s[i].state != S_FREE) {
                E.s[i].state = S_PEER_CLOSED;
            }
        }
        printf("[ec200u] PDP context deactivated by the network\n");
        return;
    }
    if (sscanf(l, "+QIOPEN: %d,%d", &id, &v) == 2) {
        if (id >= 0 && id < EC_MAX_SOCKETS && E.s[id].state == S_OPENING) {
            E.s[id].open_err = v;
            E.s[id].state = v == 0 ? S_OPEN : S_FREE;
        }
        return;
    }
    if (strcmp(l, "RDY") == 0 && E.booted) {
        /* The module restarted on its own. Almost always the supply sagging
         * under the ~2 A LTE transmit peaks. */
        E.rebooted = true;
        E.st.resets++;
        E.st.pdp_up = false;
        E.st.registered = false;
        for (int i = 0; i < EC_MAX_SOCKETS; i++) {
            if (E.s[i].state != S_FREE) {
                E.s[i].state = S_PEER_CLOSED;
            }
        }
        printf("[ec200u] the EC200U restarted by itself (RDY, %lu so far) - check its power supply: it needs "
               "~2 A peaks at 3.8-4.2 V, short thick wires and a large capacitor near VBAT\n",
               (unsigned long)E.st.resets);
    }
    if (starts(l, "+CPIN: READY")) {
        E.st.sim_ready = true;
    }
    if (!E.cmd_active) {
        return; /* other URCs (RDY, +QIND, ...) */
    }
    if (E.echo) {
        E.echo(l);
    }
    if (strcmp(l, E.final_ok) == 0) {
        E.final = 1;
    } else if (strcmp(l, "ERROR") == 0 || starts(l, "+CME ERROR") || strcmp(l, "SEND FAIL") == 0) {
        snprintf(E.info, sizeof(E.info), "%s", l);
        E.final = -1;
    } else if (E.prefix && starts(l, E.prefix)) {
        snprintf(E.info, sizeof(E.info), "%s", l);
    }
}

static void process_byte(char c)
{
    if (E.want_prompt && E.len == 0 && c == '>') {
        E.prompt = true;
        return;
    }
    if (c == '\r') {
        return;
    }
    if (c == '\n') {
        if (E.len > 0) {
            E.line[E.len] = '\0';
            E.len = 0;
            on_line(E.line);
        }
        return;
    }
    if (E.len == 0 && c == ' ' && E.prompt) {
        return; /* "> " */
    }
    if (E.len < LINE_MAX - 1u) {
        E.line[E.len++] = c;
    }
}

static void pump_once(void)
{
    int c;
    while ((c = modem_uart_getc()) >= 0) {
        process_byte((char)c);
    }
}

void ec_pump(uint32_t timeout_ms)
{
    uint32_t start = HAL_GetTick();
    do {
        pump_once();
    } while (HAL_GetTick() - start < timeout_ms);
}

/* ---- AT commands ---------------------------------------------------------------- */

static void cmd_begin(const char *final_ok, const char *prefix)
{
    pump_once(); /* settle anything already received */
    E.cmd_active = true;
    E.final_ok = final_ok;
    E.prefix = prefix;
    E.info[0] = '\0';
    E.final = 0;
}

static int cmd_wait(uint32_t timeout_ms)
{
    uint32_t start = HAL_GetTick();
    while (E.final == 0 && HAL_GetTick() - start < timeout_ms) {
        pump_once();
    }
    E.cmd_active = false;
    if (E.final == 0) {
        snprintf(E.info, sizeof(E.info), "timeout");
    }
    return E.final == 1 ? 0 : -1;
}

/* Sends "AT..." and waits for OK. A response line starting with `prefix` is
 * kept in E.info. */
static int at(const char *prefix, uint32_t timeout_ms, const char *fmt, ...)
{
    char cmd[256];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(cmd, sizeof(cmd) - 2, fmt, ap);
    va_end(ap);
    if (ec_trace) {
        printf("[at>] %s\n", cmd);
    }
    cmd[n++] = '\r';
    cmd[n++] = '\n';
    cmd_begin("OK", prefix);
    modem_uart_write(cmd, (size_t)n);
    return cmd_wait(timeout_ms);
}

int ec_at_passthrough(const char *cmd, uint32_t timeout_ms, void (*on_line_cb)(const char *line))
{
    int rc;
    E.echo = on_line_cb;
    rc = at(NULL, timeout_ms, "%s", cmd);
    E.echo = NULL;
    return rc;
}

/* ---- bring-up ------------------------------------------------------------------------ */

static bool alive(uint32_t timeout_ms)
{
    uint32_t start = HAL_GetTick();
    while (HAL_GetTick() - start < timeout_ms) {
        if (at(NULL, 500, "AT") == 0) {
            return true;
        }
    }
    return false;
}

static void grab_imei(const char *l)
{
    if (l[0] >= '0' && l[0] <= '9' && E.st.imei[0] == '\0') {
        snprintf(E.st.imei, sizeof(E.st.imei), "%s", l);
    }
}

static int fail(const char *msg)
{
    snprintf(E.st.err, sizeof(E.st.err), "%s", msg);
    return -1;
}

void ec_refresh_status(void)
{
    int a, b;
    char *q;

    if (at("+CSQ:", 1000, "AT+CSQ") == 0 && sscanf(E.info, "+CSQ: %d,%d", &a, &b) == 2) {
        E.st.rssi = a;
    }
    if (at("+COPS:", 3000, "AT+COPS?") == 0 && (q = strchr(E.info, '"')) != NULL) {
        sscanf(q + 1, "%39[^\"]", E.st.op);
    }
    if (at("+QNWINFO:", 3000, "AT+QNWINFO") == 0 && (q = strchr(E.info, '"')) != NULL) {
        sscanf(q + 1, "%15[^\"]", E.st.rat);
    }
    E.st.ip[0] = '\0';
    E.st.pdp_up = false;
    if (at("+QIACT:", 5000, "AT+QIACT?") == 0 && E.info[0] && (q = strchr(E.info, '"')) != NULL) {
        sscanf(q + 1, "%47[^\"]", E.st.ip);
        E.st.pdp_up = E.st.ip[0] != '\0';
    }
}

/* Settings the module forgets when it restarts. */
static void modem_settings(void)
{
    at(NULL, 1000, "ATE0");                                /* no command echo */
    at(NULL, 1000, "AT+CMEE=2");                           /* verbose errors */
    at(NULL, 1000, "AT+QURCCFG=\"urcport\",\"uart1\"");    /* URCs on this UART, not USB */
    at(NULL, 1000, "AT+QICFG=\"dataformat\",0,0");         /* raw (not hex) send/receive */
    at(NULL, 1000, "AT+QISDE=0");                          /* don't echo QISEND data */
}

static bool wait_sim(char *msg, size_t cap)
{
    for (int n = 0;; n++) {
        if (at("+CPIN:", 2000, "AT+CPIN?") == 0 && strstr(E.info, "READY")) {
            E.st.sim_ready = true;
            return true;
        }
        if (n >= 10) {
            snprintf(msg, cap, "SIM not ready (%s)", E.info[0] ? E.info : "no answer");
            return false;
        }
        ec_pump(1000);
    }
}

static bool wait_registered(uint32_t timeout_ms, char *msg, size_t cap)
{
    uint32_t start = HAL_GetTick();
    int n, stat;

    for (;;) {
        if (at("+CEREG:", 2000, "AT+CEREG?") == 0 && sscanf(E.info, "+CEREG: %d,%d", &n, &stat) == 2 &&
            (stat == 1 || stat == 5)) {
            E.st.registered = true;
            at(NULL, 1000, "AT+COPS=3,0"); /* operator as a name in AT+COPS? */
            return true;
        }
        if (HAL_GetTick() - start > timeout_ms) {
            snprintf(msg, cap, "no LTE registration after %lu s (last %s) - antenna, bands, SIM?",
                     (unsigned long)(timeout_ms / 1000u), E.info);
            return false;
        }
        ec_pump(2000);
    }
}

int ec_bringup(const char *apn, void (*progress)(const char *msg))
{
    char msg[128];

    memset(&E.st, 0, sizeof(E.st));
    E.st.rssi = 99;
    modem_uart_drop_rx();

    progress("checking the EC200U (AT)");
    if (!alive(3000)) {
        if (!modem_pwrkey_pulse()) {
            return fail("no answer to AT and no PWRKEY pin configured (check wiring, power, 1.8 V level shifter)");
        }
        progress("no answer - pulsed PWRKEY, waiting for the module to boot");
        if (!alive(30000)) {
            return fail("no answer to AT, even after PWRKEY (check wiring, power, baud rate, level shifter)");
        }
    }
    modem_settings();
    E.booted = true;
    E.rebooted = false;
    ec_at_passthrough("AT+GSN", 1000, grab_imei);          /* the IMEI line has no prefix */
    snprintf(E.apn, sizeof(E.apn), "%s", apn);

    progress("waiting for the SIM");
    if (!wait_sim(msg, sizeof(msg))) {
        return fail(msg);
    }
    progress("waiting for LTE registration (AT+CEREG?)");
    if (!wait_registered(MODEM_REG_TIMEOUT_MS, msg, sizeof(msg))) {
        return fail(msg);
    }

    /* Still up from before a reset (the usual case between runs)? Then leave
     * it alone: the APN can't be changed on an active context anyway. */
    ec_refresh_status();
    if (!E.st.pdp_up) {
        snprintf(msg, sizeof(msg), "activating the data connection (APN \"%s\")", apn);
        progress(msg);
        if (at(NULL, 2000, "AT+QICSGP=%d,1,\"%s\",\"\",\"\",1", MODEM_PDP_CONTEXT, apn) != 0) {
            snprintf(msg, sizeof(msg), "AT+QICSGP failed (%s)", E.info);
            return fail(msg);
        }
        if (at(NULL, 150000, "AT+QIACT=%d", MODEM_PDP_CONTEXT) != 0) {
            snprintf(msg, sizeof(msg), "PDP activation failed (%s) - check the APN", E.info);
            return fail(msg);
        }
        ec_refresh_status();
    }
    for (int i = 0; i < EC_MAX_SOCKETS; i++) {
        at(NULL, 10000, "AT+QICLOSE=%d,1", i); /* left over from before a reset */
        memset(&E.s[i], 0, sizeof(E.s[i]));
    }
    if (!E.st.pdp_up) {
        return fail("data connection has no IP address");
    }
    return 0;
}

const ec_status_t *ec_status(void)
{
    return &E.st;
}

/* ---- sockets ------------------------------------------------------------------------- */

/* The network may tear the data context down at any time (seen on Airtel
 * when the modem moves between TDD and FDD LTE cells). Bring it back before
 * a socket is opened. */
static bool ensure_pdp(void)
{
    char msg[128];

    if (E.rebooted) {
        /* The module restarted: its settings and network registration are gone. */
        E.rebooted = false;
        printf("[ec200u] modem restarted - re-applying settings and waiting for the network\n");
        if (!alive(15000)) {
            printf("[ec200u] modem doesn't answer after its restart\n");
            return false;
        }
        modem_settings();
        if (!wait_sim(msg, sizeof(msg)) || !wait_registered(120000, msg, sizeof(msg))) {
            printf("[ec200u] %s\n", msg);
            return false;
        }
        at(NULL, 2000, "AT+QICSGP=%d,1,\"%s\",\"\",\"\",1", MODEM_PDP_CONTEXT, E.apn);
    }
    ec_refresh_status();
    if (E.st.pdp_up) {
        return true;
    }
    printf("[ec200u] data connection is down - reactivating\n");
    if (at(NULL, 150000, "AT+QIACT=%d", MODEM_PDP_CONTEXT) != 0) {
        printf("[ec200u] AT+QIACT failed: %s\n", E.info);
    }
    ec_refresh_status();
    if (E.st.pdp_up) {
        printf("[ec200u] data connection back, IP %s\n", E.st.ip);
    }
    return E.st.pdp_up;
}

static int sock_open_once(bool udp, const char *host, uint16_t port, uint32_t timeout_ms);

int ec_sock_open(bool udp, const char *host, uint16_t port, uint32_t timeout_ms)
{
    int id;

    if ((!E.st.pdp_up || E.rebooted) && !ensure_pdp()) {
        return -1;
    }
    id = sock_open_once(udp, host, port, timeout_ms);
    if (id < 0 && ensure_pdp()) {
        id = sock_open_once(udp, host, port, timeout_ms); /* the context had gone: once more */
    }
    return id;
}

static int sock_open_once(bool udp, const char *host, uint16_t port, uint32_t timeout_ms)
{
    uint32_t start;
    int id;

    for (id = 0; id < EC_MAX_SOCKETS && E.s[id].state != S_FREE; id++) {
    }
    if (id == EC_MAX_SOCKETS) {
        return -1;
    }
    memset(&E.s[id], 0, sizeof(E.s[id]));
    E.s[id].udp = udp;
    E.s[id].state = S_OPENING;
    /* access_mode 1 = direct push */
    if (at(NULL, 5000, "AT+QIOPEN=%d,%d,\"%s\",\"%s\",%u,0,1", MODEM_PDP_CONTEXT, id, udp ? "UDP" : "TCP", host,
           (unsigned)port) != 0) {
        printf("[ec200u] AT+QIOPEN failed: %s\n", E.info);
        E.s[id].state = S_FREE;
        return -1;
    }
    start = HAL_GetTick();
    while (E.s[id].state == S_OPENING && HAL_GetTick() - start < timeout_ms) {
        pump_once();
    }
    if (E.s[id].state != S_OPEN) {
        printf("[ec200u] %s %s:%u failed: %s\n", udp ? "UDP" : "TCP", host, (unsigned)port,
               E.s[id].state == S_OPENING ? "no +QIOPEN (timeout)" : "error");
        if (E.s[id].open_err) {
            printf("[ec200u] +QIOPEN error %d (565 = DNS failure, 566 = connect failed)\n", E.s[id].open_err);
        }
        at(NULL, 10000, "AT+QICLOSE=%d,1", id);
        E.s[id].state = S_FREE;
        return -1;
    }
    return id;
}

static int send_chunk(int id, const uint8_t *data, size_t len)
{
    char cmd[32];
    uint32_t start;
    int n = snprintf(cmd, sizeof(cmd), "AT+QISEND=%d,%u\r\n", id, (unsigned)len);

    if (ec_trace) {
        printf("[at>] AT+QISEND=%d,%u + %u data bytes\n", id, (unsigned)len, (unsigned)len);
    }
    cmd_begin("SEND OK", NULL);
    E.want_prompt = true;
    E.prompt = false;
    modem_uart_write(cmd, (size_t)n);
    start = HAL_GetTick();
    while (!E.prompt && E.final == 0 && HAL_GetTick() - start < 5000u) {
        pump_once();
    }
    E.want_prompt = false;
    if (!E.prompt) {
        E.cmd_active = false;
        return -1;
    }
    modem_uart_write(data, len);
    return cmd_wait(10000);
}

int ec_sock_send(int id, const void *data, size_t len)
{
    const uint8_t *p = data;
    size_t done = 0;

    if (id < 0 || id >= EC_MAX_SOCKETS || E.s[id].state != S_OPEN) {
        return -1;
    }
    if (E.s[id].udp && len > QISEND_MAX) {
        return -1; /* a datagram can't be split */
    }
    while (done < len) {
        size_t n = len - done > QISEND_MAX ? QISEND_MAX : len - done;
        if (send_chunk(id, p + done, n) != 0) {
            printf("[ec200u] AT+QISEND failed: %s\n", E.info);
            return -1;
        }
        done += n;
    }
    return (int)len;
}

int ec_sock_recv(int id, void *buf, size_t cap, uint32_t timeout_ms)
{
    sock_t *s;
    uint8_t *out = buf;
    uint32_t start = HAL_GetTick();
    size_t n = 0;

    if (id < 0 || id >= EC_MAX_SOCKETS || E.s[id].state == S_FREE) {
        return -1;
    }
    s = &E.s[id];
    for (;;) {
        pump_once();
        if (rb_used(s) > 0) {
            break;
        }
        if (s->state == S_PEER_CLOSED) {
            return -2;
        }
        if (HAL_GetTick() - start >= timeout_ms) {
            return 0;
        }
    }
    if (s->udp) {
        size_t len = (size_t)rb_get(s) << 8;
        len |= rb_get(s);
        for (size_t i = 0; i < len; i++) {
            uint8_t c = rb_get(s);
            if (i < cap) {
                out[n++] = c;
            }
        }
        return (int)n;
    }
    while (n < cap && rb_used(s) > 0) {
        out[n++] = rb_get(s);
    }
    return (int)n;
}

void ec_sock_close(int id)
{
    if (id < 0 || id >= EC_MAX_SOCKETS || E.s[id].state == S_FREE) {
        return;
    }
    at(NULL, 11000, "AT+QICLOSE=%d,10", id);
    E.s[id].state = S_FREE;
}
