/*
 * STM32L452RE-P + EC200U firmware for the MQTT vs LwM2M playground.
 *
 * The board runs the *same* mqtt_device / lwm2m_device code as the PC (their
 * main() is compiled as pg_mqtt_main / pg_lwm2m_main), over the EC200U's
 * cellular data connection instead of a PC socket.
 *
 * It is driven over the ST-LINK virtual COM port, one command per line:
 *
 *   time <epoch_ms> [utc_offset_min]   set the wall clock (the lab does this)
 *   run mqtt  <mqtt_device options>    e.g. run mqtt --host x.ply.gg --port 1234 --qos 1
 *   run lwm2m <lwm2m_device options>
 *   status                             modem / network state as JSON
 *   apn <name>                         change the APN (kept until power-off)
 *   at <command>                       raw AT command, e.g. "at AT+CSQ"
 *   retry                              redo the modem bring-up
 *   reset                              reboot the board
 *
 * While a run is active, lines are device commands instead (stop, pause,
 * publish, notify, ...), exactly as the dashboard sends them on the PC.
 * When the run ends the board resets itself, so each run starts clean.
 *
 * Lines starting with '{' are JSON for the lab; everything else is console
 * text, so a plain serial terminal at VCP_BAUD shows what is happening.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "ec200u.h"
#include "hw_config.h"
#include "pal.h"
#include "pal_hw.h"

int pg_mqtt_main(int argc, char **argv);
int pg_lwm2m_main(int argc, char **argv);

#define FW_VERSION "1.0.0"
#define PG_STR2(x) #x
#define PG_STR(x) PG_STR2(x)

/* Survives the reset at the end of each run. */
static struct {
    uint32_t magic;
    char apn[64];
    uint32_t runs;
    uint32_t trace;
} keep __attribute__((section(".noinit")));
#define KEEP_MAGIC 0x50474858u /* change when the struct changes */

static bool modem_ok;
static char modem_err[128];

/* ---- JSON lines for the lab ------------------------------------------------- */

static void hw_json(const char *state, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void hw_json(const char *state, const char *fmt, ...)
{
    char extra[512] = "";
    va_list ap;

    if (fmt) {
        va_start(ap, fmt);
        vsnprintf(extra, sizeof(extra), fmt, ap);
        va_end(ap);
    }
    fflush(stdout);
    printf("{\"k\":\"hw\",\"state\":\"%s\",\"t\":%llu%s%s}\n", state, (unsigned long long)pal_wall_ms(),
           extra[0] ? "," : "", extra);
    fflush(stdout);
}

static void json_str(char *out, size_t cap, const char *s)
{
    size_t o = 0;
    for (; s && *s && o + 2 < cap; s++) {
        if (*s == '"' || *s == '\\') {
            out[o++] = '\\';
        }
        out[o++] = (*s >= 0x20 && *s < 0x7f) ? *s : '?';
    }
    out[o] = '\0';
}

static void send_status(const char *state)
{
    const ec_status_t *st = ec_status();
    char op[48], err[160];

    json_str(op, sizeof(op), st->op);
    json_str(err, sizeof(err), modem_ok ? "" : modem_err);
    hw_json(state,
            "\"fw\":\"" FW_VERSION "\",\"board\":\"NUCLEO-L452RE-P\",\"modem\":\"EC200U\",\"modem_ok\":%s,"
            "\"apn\":\"%s\",\"sim\":%s,\"registered\":%s,\"pdp\":%s,\"rssi\":%d,\"operator\":\"%s\","
            "\"rat\":\"%s\",\"ip\":\"%s\",\"imei\":\"%s\",\"err\":\"%s\",\"uart_overruns\":%lu,"
            "\"modem_resets\":%lu",
            modem_ok ? "true" : "false", keep.apn, st->sim_ready ? "true" : "false",
            st->registered ? "true" : "false", st->pdp_up ? "true" : "false", st->rssi, op, st->rat, st->ip,
            st->imei, err, (unsigned long)modem_uart_overruns(), (unsigned long)st->resets);
}

static void progress(const char *msg)
{
    char esc[200];
    printf("[modem] %s\n", msg);
    json_str(esc, sizeof(esc), msg);
    hw_json("modem", "\"msg\":\"%s\"", esc);
}

static void bringup(void)
{
    board_led(false);
    modem_ok = ec_bringup(keep.apn, progress) == 0;
    if (modem_ok) {
        const ec_status_t *st = ec_status();
        printf("[modem] ready: %s %s, signal %d (CSQ), IP %s\n", st->op, st->rat, st->rssi, st->ip);
        modem_err[0] = '\0';
        board_led(true);
        send_status("ready");
    } else {
        snprintf(modem_err, sizeof(modem_err), "%s", ec_status()->err);
        printf("[modem] FAILED: %s\n", modem_err);
        send_status("modem_error");
    }
}

/* ---- commands ------------------------------------------------------------------ */

static void print_line(const char *l)
{
    printf("  %s\n", l);
}

static void run(char *args)
{
    static char *argv[48];
    int argc = 0, code;
    char *kind = strtok(args, " ");
    char *tok;

    if (kind == NULL || (strcmp(kind, "mqtt") != 0 && strcmp(kind, "lwm2m") != 0)) {
        printf("usage: run mqtt|lwm2m [options]\n");
        return;
    }
    if (!modem_ok) {
        hw_json("exit", "\"kind\":\"%s\",\"code\":1,\"msg\":\"modem not ready\"", kind);
        return;
    }
    argv[argc++] = strcmp(kind, "mqtt") == 0 ? "mqtt_device" : "lwm2m_device";
    while ((tok = strtok(NULL, " ")) != NULL && argc < (int)(sizeof(argv) / sizeof(argv[0])) - 1) {
        argv[argc++] = tok;
    }
    argv[argc] = NULL;

    keep.runs++;
    hw_json("running", "\"kind\":\"%s\"", kind);
    code = strcmp(kind, "mqtt") == 0 ? pg_mqtt_main(argc, argv) : pg_lwm2m_main(argc, argv);
    fflush(stdout);
    hw_json("exit", "\"kind\":\"%s\",\"code\":%d", kind, code);
    printf("[hw] run finished - resetting for a clean start\n");
    fflush(stdout);
    board_reset();
}

static void command(char *line)
{
    char *arg = strchr(line, ' ');
    if (arg) {
        *arg++ = '\0';
        while (*arg == ' ') arg++;
    } else {
        arg = line + strlen(line);
    }

    if (strcmp(line, "time") == 0) {
        char *end;
        unsigned long long ms = strtoull(arg, &end, 10);
        long off = strtol(end, NULL, 10);
        pal_hw_set_wall((uint64_t)ms, (int32_t)off);
    } else if (strcmp(line, "run") == 0) {
        run(arg);
    } else if (strcmp(line, "status") == 0) {
        if (modem_ok) {
            ec_refresh_status();
        }
        send_status(modem_ok ? "ready" : "modem_error");
    } else if (strcmp(line, "apn") == 0 && arg[0]) {
        snprintf(keep.apn, sizeof(keep.apn), "%s", arg);
        printf("APN set to \"%s\" - reconnecting\n", keep.apn);
        ec_at_passthrough("AT+QIDEACT=" PG_STR(MODEM_PDP_CONTEXT), 40000, NULL); /* drop the old context */
        bringup();
    } else if (strcmp(line, "at") == 0 && arg[0]) {
        int rc = ec_at_passthrough(arg, 10000, print_line);
        printf("%s\n", rc == 0 ? "OK" : "ERROR / timeout");
    } else if (strcmp(line, "trace") == 0) {
        keep.trace = strcmp(arg, "off") != 0 && strcmp(arg, "0") != 0;
        ec_trace = keep.trace != 0;
        printf("AT trace %s\n", ec_trace ? "on (kept until power-off)" : "off");
    } else if (strcmp(line, "retry") == 0) {
        bringup();
    } else if (strcmp(line, "reset") == 0) {
        board_reset();
    } else if (strcmp(line, "stop") == 0) {
        /* a stop for a run that already ended: just confirm we're idle */
        send_status(modem_ok ? "ready" : "modem_error");
    } else if (line[0]) {
        printf("commands: time <ms> [offset_min] | run mqtt|lwm2m [options] | status | apn <name> | "
               "at <cmd> | trace on|off | retry | reset\n");
    }
}

/* ---- mbedTLS entropy (MBEDTLS_ENTROPY_HARDWARE_ALT) ------------------------------ */

int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen)
{
    (void)data;
    if (board_rng(output, len) != 0) {
        *olen = 0;
        return -1;
    }
    *olen = len;
    return 0;
}

/* ---- main ------------------------------------------------------------------------- */

int main(void)
{
    char line[640];
    uint32_t last_blink = 0;

    board_init();
    if (keep.magic != KEEP_MAGIC) {
        memset(&keep, 0, sizeof(keep));
        keep.magic = KEEP_MAGIC;
        snprintf(keep.apn, sizeof(keep.apn), "%s", MODEM_APN);
    }
    ec_trace = keep.trace != 0;
    printf("\n\n=== MQTT vs LwM2M playground - NUCLEO-L452RE-P + EC200U (fw " FW_VERSION ") ===\n");
    printf("type 'help' for commands; the lab drives this port automatically\n");
    hw_json("boot", "\"fw\":\"" FW_VERSION "\",\"runs\":%lu", (unsigned long)keep.runs);
    bringup();

    for (;;) {
        if (vcp_read_line(line, sizeof(line))) {
            command(line);
        }
        ec_pump(0);
        if (!modem_ok && HAL_GetTick() - last_blink > 250u) { /* fast blink = modem problem */
            static bool on;
            board_led(on = !on);
            last_blink = HAL_GetTick();
        }
    }
}
