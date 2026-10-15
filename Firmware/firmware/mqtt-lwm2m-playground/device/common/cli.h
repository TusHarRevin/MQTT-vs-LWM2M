#ifndef CLI_H
#define CLI_H

/* Command-line options shared by all simulated devices. */

#include <stdbool.h>
#include <stdint.h>

#include "link.h"
#include "netsim.h"

typedef struct
{
    const char *host;
    uint16_t port;            /* 0 = protocol default */
    const char *id;           /* device id / LwM2M endpoint name */
    uint32_t interval_ms;
    uint32_t count;           /* 0 = run until stopped */
    uint32_t payload;         /* target payload bytes, 0 = natural size */
    uint64_t seed;
    uint32_t timeout_ms;      /* per-request response timeout */

    bool secure;
    const char *ca_file;
    const char *tls_name;
    bool tls12;
    const char *psk_id;
    const char *psk_hex;

    netsim_cfg_t net;
    const char *profile;

    const char *lab_host;     /* where events go */
    uint16_t lab_port;        /* 0 = don't send events */
    uint16_t ctl_port;        /* 0 = no control channel */
    const char *results_dir;
    char run_id[48];
} pg_opts_t;

typedef struct
{
    const char *name;    /* "--qos" */
    bool takes_value;
    const char *help;
} cli_opt_t;

/* Parses common options and validates that every argument is either a
 * common option or listed in `extra`. Prints usage and returns false on
 * error or --help. */
bool cli_parse(int argc, char **argv, pg_opts_t *o, const char *prog, const char *summary,
               const cli_opt_t *extra);
/* Value of a device-specific option, or `def`. Flags return "1". */
const char *cli_get(int argc, char **argv, const char *name, const char *def);
/* "10s", "1500ms", "2m" or plain milliseconds. */
uint32_t cli_duration_ms(const char *s);

#endif /* CLI_H */
