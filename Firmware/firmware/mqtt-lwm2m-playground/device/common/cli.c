#include "cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pal.h"

static const cli_opt_t common_opts[] = {
    { "--host", true, "server host (default 127.0.0.1)" },
    { "--port", true, "server port (default depends on protocol/security)" },
    { "--id", true, "device id / endpoint name" },
    { "--interval", true, "time between messages: 10s, 500ms, 2m (default 10s)" },
    { "--count", true, "stop after N messages (default 0 = run until stopped)" },
    { "--payload", true, "pad payload to N bytes (default 0 = natural size)" },
    { "--seed", true, "sensor/netsim random seed (default 1)" },
    { "--timeout", true, "response timeout (default 10s)" },
    { "--secure", false, "enable TLS (MQTT) / DTLS-PSK (LwM2M)" },
    { "--ca", true, "TLS: CA certificate PEM used to verify the server" },
    { "--tls-name", true, "TLS: expected server name (default = host)" },
    { "--tls12", false, "TLS: do not offer TLS 1.3" },
    { "--psk-id", true, "DTLS: PSK identity (default = device id)" },
    { "--psk", true, "DTLS: PSK as hex (default 000102...0f, lab only!)" },
    { "--profile", true, "network profile: excellent|good|poor|very-poor|custom" },
    { "--latency", true, "simulated one-way latency in ms" },
    { "--loss", true, "simulated packet loss in percent (0-100)" },
    { "--rto", true, "TCP retransmission penalty model in ms (default 200)" },
    { "--lab", true, "lab event sink host:port (default 127.0.0.1:7000, 'off' to disable)" },
    { "--ctl-port", true, "local UDP control port (0 = off)" },
    { "--results", true, "directory for benchmark CSV files (default ./results)" },
    { "--run", true, "run id used in results (default: generated)" },
    { "--help", false, "show this help" },
    { NULL, false, NULL },
};

static const cli_opt_t *find(const cli_opt_t *list, const char *name)
{
    for (; list && list->name; list++) {
        if (strcmp(list->name, name) == 0) {
            return list;
        }
    }
    return NULL;
}

static void usage(const char *prog, const char *summary, const cli_opt_t *extra)
{
    const cli_opt_t *lists[2] = { extra, common_opts };
    printf("%s - %s\n\nUsage: %s [options]\n", prog, summary, prog);
    for (int l = 0; l < 2; l++) {
        printf("\n%s options:\n", l == 0 ? "Device" : "Common");
        for (const cli_opt_t *o = lists[l]; o && o->name; o++) {
            char left[40];
            snprintf(left, sizeof(left), "%s%s", o->name, o->takes_value ? " <v>" : "");
            printf("  %-22s %s\n", left, o->help);
        }
    }
}

const char *cli_get(int argc, char **argv, const char *name, const char *def)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], name) == 0) {
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                return argv[i + 1];
            }
            /* negative numbers are values too */
            if (i + 1 < argc && argv[i + 1][0] == '-' && argv[i + 1][1] >= '0' &&
                argv[i + 1][1] <= '9') {
                return argv[i + 1];
            }
            return "1";
        }
    }
    return def;
}

uint32_t cli_duration_ms(const char *s)
{
    char *end = NULL;
    double v = strtod(s, &end);
    if (end == NULL || *end == '\0' || strcmp(end, "ms") == 0) {
        return (uint32_t)v;
    }
    if (strcmp(end, "s") == 0) {
        return (uint32_t)(v * 1000.0);
    }
    if (strcmp(end, "m") == 0 || strcmp(end, "min") == 0) {
        return (uint32_t)(v * 60000.0);
    }
    return (uint32_t)v;
}

bool cli_parse(int argc, char **argv, pg_opts_t *o, const char *prog, const char *summary,
               const cli_opt_t *extra)
{
    const char *v;

    for (int i = 1; i < argc; i++) {
        const cli_opt_t *opt = find(common_opts, argv[i]);
        if (opt == NULL) {
            opt = find(extra, argv[i]);
        }
        if (opt == NULL) {
            fprintf(stderr, "%s: unknown option '%s' (try --help)\n", prog, argv[i]);
            return false;
        }
        if (strcmp(opt->name, "--help") == 0) {
            usage(prog, summary, extra);
            return false;
        }
        if (opt->takes_value) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s: %s needs a value\n", prog, opt->name);
                return false;
            }
            i++;
        }
    }

    o->host = cli_get(argc, argv, "--host", "127.0.0.1");
    o->port = (uint16_t)atoi(cli_get(argc, argv, "--port", "0"));
    o->interval_ms = cli_duration_ms(cli_get(argc, argv, "--interval", "10s"));
    o->count = (uint32_t)strtoul(cli_get(argc, argv, "--count", "0"), NULL, 10);
    o->payload = (uint32_t)strtoul(cli_get(argc, argv, "--payload", "0"), NULL, 10);
    o->seed = strtoull(cli_get(argc, argv, "--seed", "1"), NULL, 10);
    o->timeout_ms = cli_duration_ms(cli_get(argc, argv, "--timeout", "10s"));
    o->secure = cli_get(argc, argv, "--secure", NULL) != NULL;
    o->ca_file = cli_get(argc, argv, "--ca", NULL);
    o->tls_name = cli_get(argc, argv, "--tls-name", NULL);
    o->tls12 = cli_get(argc, argv, "--tls12", NULL) != NULL;
    o->psk_hex = cli_get(argc, argv, "--psk", "000102030405060708090a0b0c0d0e0f");

    memset(&o->net, 0, sizeof(o->net));
    o->profile = cli_get(argc, argv, "--profile", "custom");
    if (!netsim_profile(o->profile, &o->net)) {
        fprintf(stderr, "%s: unknown profile '%s'\n", prog, o->profile);
        return false;
    }
    if ((v = cli_get(argc, argv, "--latency", NULL)) != NULL) {
        o->net.latency_ms = (uint32_t)atoi(v);
    }
    if ((v = cli_get(argc, argv, "--loss", NULL)) != NULL) {
        o->net.loss = atof(v) / 100.0;
    }
    o->net.rto_ms = (uint32_t)atoi(cli_get(argc, argv, "--rto", "200"));
    o->net.seed = o->seed;

    v = cli_get(argc, argv, "--lab", "127.0.0.1:7000");
    if (strcmp(v, "off") == 0) {
        o->lab_host = NULL;
        o->lab_port = 0;
    } else {
        static char host[64];
        const char *colon = strrchr(v, ':');
        if (colon) {
            snprintf(host, sizeof(host), "%.*s", (int)(colon - v), v);
            o->lab_port = (uint16_t)atoi(colon + 1);
        } else {
            snprintf(host, sizeof(host), "%s", v);
            o->lab_port = 7000;
        }
        o->lab_host = host;
    }
    o->results_dir = cli_get(argc, argv, "--results", "results");
    v = cli_get(argc, argv, "--run", NULL);
    if (v) {
        snprintf(o->run_id, sizeof(o->run_id), "%s", v);
    } else {
        snprintf(o->run_id, sizeof(o->run_id), "%s-%llu", prog,
                 (unsigned long long)(pal_wall_ms() / 1000));
    }
    o->id = cli_get(argc, argv, "--id", NULL);
    o->psk_id = cli_get(argc, argv, "--psk-id", NULL);
    return true;
}
