/*
 * ota_device - SIMULATED over-the-air update, for learning only.
 *
 *   Server --(firmware available: manifest)--> Device
 *   Device --(download in chunks, HTTP Range)--> Storage (a file = "slot B")
 *   Device --(verify SHA-256 against manifest)--> activate new version
 *
 * The download runs over the same link/netsim layer as the protocols, so
 * latency and loss affect it the same way. Chunked range requests mimic how a
 * modem-attached MCU fetches an image (one request per block, e.g. CoAP
 * block-wise or AT+QHTTPGET with ranges).
 *
 * This does NOT model STM32 flash: no erase/write timing, no dual-bank swap,
 * no bootloader, no rollback. Those come later, on real hardware.
 *
 *   ./build/ota_device --image firmware_v2.bin --port 8000
 *   ./build/ota_device --action reset
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli.h"
#include "control.h"
#include "events.h"
#include "link.h"
#include "mbedtls/sha256.h"
#include "netsim.h"
#include "pal.h"

static const cli_opt_t ota_opts[] = {
    { "--image", true, "image name on the lab server (default firmware_v2.bin; fw_<N>k.bin = synthetic N KiB)" },
    { "--chunk", true, "bytes per range request (default 1024)" },
    { "--action", true, "update (default) or reset (back to v1, wipe slot)" },
    { NULL, false, NULL },
};

static struct {
    pg_opts_t o;
    netsim_t sim;
    link_t link;
    bool link_up;
    char storage[600], slot[700], active[700];
    uint8_t buf[70000];
    size_t buf_len;
    bool interrupted;
} g;

static void ota_ev(const char *from, const char *to, const char *type, double lat, const char *fmt, ...)
{
    char detail[300];
    ev_t e = EV_INIT;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(detail, sizeof(detail), fmt, ap);
    va_end(ap);
    e.proto = "OTA";
    e.from = from;
    e.to = to;
    e.type = type;
    e.lat_ms = lat;
    e.detail = detail;
    ev_emit(&e);
}

static void read_active(char *ver, size_t cap)
{
    FILE *f = fopen(g.active, "r");
    snprintf(ver, cap, "1.0.0");
    if (f) {
        if (fgets(ver, (int)cap, f)) {
            ver[strcspn(ver, "\r\n")] = '\0';
        }
        fclose(f);
    }
}

static bool check_interrupt(void)
{
    char cmd[64];
    while (ctl_poll(cmd, sizeof(cmd)) > 0) {
        if (strcmp(cmd, "interrupt") == 0 || strcmp(cmd, "stop") == 0) {
            g.interrupted = true;
        }
    }
    return g.interrupted;
}

static int ensure_link(void)
{
    if (g.link_up) {
        return 0;
    }
    if (link_open(&g.link, LINK_TCP, g.o.host, g.o.port, NULL, &g.sim, g.o.timeout_ms) != 0) {
        ev_log("NET", "%s", g.link.err);
        return -1;
    }
    g.link_up = true;
    return 0;
}

static void drop_link(void)
{
    if (g.link_up) {
        link_close(&g.link);
        g.link_up = false;
    }
}

/* One HTTP/1.1 GET on the kept-alive link. Body goes to g.buf. Returns the
 * HTTP status or -1. */
static int http_get(const char *path, long from, long to)
{
    char req[512], head[2048];
    size_t hl = 0, clen = 0;
    char *end = NULL, *cl;
    uint64_t deadline;
    int n, status = 0;

    if (ensure_link() != 0) {
        return -1;
    }
    if (from >= 0) {
        n = snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHost: %s\r\nRange: bytes=%ld-%ld\r\n\r\n", path,
                     g.o.host, from, to);
    } else {
        n = snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHost: %s\r\n\r\n", path, g.o.host);
    }
    if (link_send(&g.link, (const uint8_t *)req, (size_t)n) != n) {
        drop_link();
        return -1;
    }
    deadline = pal_now_us() + (uint64_t)g.o.timeout_ms * 1000ULL;
    /* headers: read byte-wise-ish until CRLFCRLF (link buffers internally) */
    while (end == NULL) {
        uint64_t now = pal_now_us();
        if (now >= deadline || hl >= sizeof(head) - 1) {
            drop_link();
            return -1;
        }
        n = link_recv(&g.link, (uint8_t *)head + hl, 1, (uint32_t)((deadline - now) / 1000 + 1));
        if (n < 0) {
            drop_link();
            return -1;
        }
        hl += (size_t)n;
        head[hl] = '\0';
        end = strstr(head, "\r\n\r\n");
    }
    sscanf(head, "HTTP/%*s %d", &status);
    cl = strstr(head, "Content-Length:");
    if (cl) {
        clen = (size_t)strtoul(cl + 15, NULL, 10);
    }
    if (clen > sizeof(g.buf)) {
        drop_link();
        return -1;
    }
    g.buf_len = 0;
    while (g.buf_len < clen) {
        uint64_t now = pal_now_us();
        if (now >= deadline) {
            drop_link();
            return -1;
        }
        n = link_recv(&g.link, g.buf + g.buf_len, clen - g.buf_len, (uint32_t)((deadline - now) / 1000 + 1));
        if (n < 0) {
            drop_link();
            return -1;
        }
        g.buf_len += (size_t)n;
    }
    return status;
}

static void report(const char *phase, uint32_t done, uint32_t total, const char *version)
{
    ev_json("ota", "\"phase\":\"%s\",\"done\":%u,\"total\":%u,\"pct\":%.1f,\"version\":\"%s\","
                   "\"tx_bytes\":%llu,\"rx_bytes\":%llu",
            phase, done, total, total ? 100.0 * done / total : 0.0, version,
            (unsigned long long)g.link.st.tx_bytes, (unsigned long long)g.link.st.rx_bytes);
}

static void summary(const char *image, const char *version, uint32_t size, bool ok, const char *reason,
                    double download_ms, double verify_ms, double total_ms, uint32_t chunks, uint32_t chunk,
                    uint32_t retries)
{
    char esc[200];
    json_escape(esc, sizeof(esc), reason, 150);
    ev_json("run",
            "\"summary\":{\"run\":\"%s\",\"proto\":\"OTA\",\"started\":%llu,\"ended\":%llu,\"image\":\"%s\","
            "\"version\":\"%s\",\"size\":%u,\"ok\":%s,\"success_pct\":%d,\"reason\":\"%s\","
            "\"download_ms\":%.1f,\"verify_ms\":%.1f,\"total_ms\":%.1f,\"wire_tx\":%llu,\"wire_rx\":%llu,"
            "\"chunks\":%u,\"chunk\":%u,\"retries\":%u,\"latency_ms\":%u,\"loss_pct\":%.1f,"
            "\"profile\":\"%s\",\"simulated\":true}",
            g.o.run_id, (unsigned long long)(pal_wall_ms() - (uint64_t)total_ms), (unsigned long long)pal_wall_ms(),
            image, version, size, ok ? "true" : "false", ok ? 100 : 0, esc, download_ms, verify_ms, total_ms,
            (unsigned long long)g.link.st.tx_bytes, (unsigned long long)g.link.st.rx_bytes, chunks, chunk,
            retries, g.sim.cfg.latency_ms, g.sim.cfg.loss * 100.0, g.o.profile);
    printf("\n==== SIMULATED OTA %s ====\n%s  image %s v%s  %u bytes\n", g.o.run_id,
           ok ? "COMPLETED" : "FAILED", image, version, size);
    printf("download %.1f ms  verify %.1f ms  total %.1f ms  chunks %u x %u B  retries %u\n", download_ms,
           verify_ms, total_ms, chunks, chunk, retries);
    printf("wire bytes  sent %llu  received %llu%s%s\n\n", (unsigned long long)g.link.st.tx_bytes,
           (unsigned long long)g.link.st.rx_bytes, ok ? "" : "  reason: ", ok ? "" : reason);
}

int main(int argc, char **argv)
{
    char image[128], version[32] = "?", sha_hex[65] = "", cur[32], path[256];
    uint32_t size = 0, chunk, done = 0, chunks = 0, retries = 0;
    uint64_t t0, t_dl, t_v;
    FILE *slot;
    int st;

    setvbuf(stdout, NULL, _IOLBF, 0);
    if (!cli_parse(argc, argv, &g.o, "ota_device", "SIMULATED firmware update over the lab server", ota_opts)) {
        return 2;
    }
    pal_net_init();
    if (g.o.port == 0) {
        g.o.port = 8000;
    }
    snprintf(image, sizeof(image), "%s", cli_get(argc, argv, "--image", "firmware_v2.bin"));
    chunk = (uint32_t)atoi(cli_get(argc, argv, "--chunk", "1024"));
    if (chunk < 64 || chunk > 65536) {
        fprintf(stderr, "--chunk must be 64..65536\n");
        return 2;
    }
    g.o.ctl_port = (uint16_t)atoi(cli_get(argc, argv, "--ctl-port", "7103"));
    ev_init("ota-dev-1", "ota", g.o.lab_host, g.o.lab_port);
    ev_set_run(g.o.run_id);
    ctl_open(g.o.ctl_port);
    netsim_init(&g.sim, &g.o.net);

    pal_mkdir(g.o.results_dir);
    snprintf(g.storage, sizeof(g.storage), "%s/ota_storage", g.o.results_dir);
    pal_mkdir(g.storage);
    snprintf(g.slot, sizeof(g.slot), "%s/slot_b.bin", g.storage);
    snprintf(g.active, sizeof(g.active), "%s/active_version.txt", g.storage);
    read_active(cur, sizeof(cur));

    if (strcmp(cli_get(argc, argv, "--action", "update"), "reset") == 0) {
        FILE *f;
        remove(g.slot);
        f = fopen(g.active, "w");
        if (f) {
            fputs("1.0.0\n", f);
            fclose(f);
        }
        ev_log("OTA", "SIMULATED OTA reset: slot erased, running version back to 1.0.0");
        report("reset", 0, 0, "1.0.0");
        return 0;
    }

    ev_log("OTA", "SIMULATED OTA start - running v%s, server %s:%u, image %s, chunk %u B", cur, g.o.host,
           (unsigned)g.o.port, image, chunk);
    if (netsim_active(&g.sim)) {
        ev_log("NET", "simulated network: %u ms one-way latency, %.1f %% loss (app-level)",
               g.sim.cfg.latency_ms, g.sim.cfg.loss * 100.0);
    }
    t0 = pal_now_us();

    /* 1. firmware available? */
    snprintf(path, sizeof(path), "/ota/manifest?image=%s", image);
    st = http_get(path, -1, -1);
    if (st != 200) {
        ev_log("OTA", "manifest request failed (%d) - is the lab running on port %u?", st, (unsigned)g.o.port);
        summary(image, version, 0, false, "manifest unavailable", 0, 0, pal_ms_since(t0), 0, chunk, 0);
        return 1;
    }
    g.buf[g.buf_len < sizeof(g.buf) ? g.buf_len : sizeof(g.buf) - 1] = '\0';
    {
        const char *p;
        if ((p = strstr((char *)g.buf, "\"size\":")) != NULL) size = (uint32_t)strtoul(p + 7, NULL, 10);
        if ((p = strstr((char *)g.buf, "\"sha256\":\"")) != NULL) sscanf(p + 10, "%64[0-9a-f]", sha_hex);
        if ((p = strstr((char *)g.buf, "\"version\":\"")) != NULL) sscanf(p + 11, "%31[^\"]", version);
    }
    ota_ev("SERVER", "DEVICE", "FIRMWARE AVAILABLE", pal_ms_since(t0), "%s v%s, %u bytes, sha256 %.16s...",
           image, version, size, sha_hex);
    report("available", 0, size, version);

    /* 2. download into slot B */
    slot = fopen(g.slot, "wb");
    if (slot == NULL) {
        summary(image, version, size, false, "cannot open storage slot", 0, 0, pal_ms_since(t0), 0, chunk, 0);
        return 1;
    }
    ota_ev("DEVICE", "SERVER", "DOWNLOAD START", -1, "%u chunks of %u B", (size + chunk - 1) / chunk, chunk);
    t_dl = pal_now_us();
    while (done < size) {
        uint32_t want = size - done < chunk ? size - done : chunk;
        uint64_t c0 = pal_now_us();
        int tries = 0;
        if (check_interrupt()) {
            break;
        }
        snprintf(path, sizeof(path), "/ota/%s", image);
        while ((st = http_get(path, (long)done, (long)(done + want - 1))) != 206 && tries < 3) {
            tries++;
            retries++;
            ev_log("OTA", "chunk at %u failed (%d), retry %d/3", done, st, tries);
            pal_sleep_ms(200);
            if (check_interrupt()) {
                break;
            }
        }
        if (st != 206 || g.buf_len != want) {
            break;
        }
        fwrite(g.buf, 1, want, slot);
        done += want;
        chunks++;
        report("download", done, size, version);
        if (chunks == 1 || done == size || (done * 10 / size) != ((done - want) * 10 / size)) {
            ota_ev("SERVER", "DEVICE", "CHUNK", pal_ms_since(c0), "%u/%u bytes (%.0f %%)", done, size,
                   100.0 * done / size);
        }
    }
    fclose(slot);
    drop_link();

    if (done < size) {
        const char *why = g.interrupted ? "interrupted by user" : "download failed (network)";
        ota_ev("DEVICE", "STORAGE", g.interrupted ? "INTERRUPTED" : "FAILED", -1,
               "%s at %u/%u bytes - still running v%s", why, done, size, cur);
        report(g.interrupted ? "interrupted" : "failed", done, size, version);
        summary(image, version, size, false, why, pal_ms_since(t_dl), 0, pal_ms_since(t0), chunks, chunk, retries);
        return 1;
    }

    /* 3. verify */
    t_v = pal_now_us();
    {
        unsigned char hash[32];
        char hex[65];
        mbedtls_sha256_context ctx;
        FILE *f = fopen(g.slot, "rb");
        size_t n;
        mbedtls_sha256_init(&ctx);
        mbedtls_sha256_starts(&ctx, 0);
        while (f && (n = fread(g.buf, 1, sizeof(g.buf), f)) > 0) {
            mbedtls_sha256_update(&ctx, g.buf, n);
        }
        if (f) fclose(f);
        mbedtls_sha256_finish(&ctx, hash);
        mbedtls_sha256_free(&ctx);
        for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", hash[i]);
        report("verify", done, size, version);
        if (strcmp(hex, sha_hex) != 0) {
            ota_ev("STORAGE", "DEVICE", "VERIFY FAILED", pal_ms_since(t_v), "sha256 %.16s... != manifest %.16s...",
                   hex, sha_hex);
            summary(image, version, size, false, "SHA-256 mismatch", pal_ms_since(t_dl), pal_ms_since(t_v),
                    pal_ms_since(t0), chunks, chunk, retries);
            return 1;
        }
        ota_ev("STORAGE", "DEVICE", "VERIFY OK", pal_ms_since(t_v), "sha256 %.16s... matches the manifest", hex);
    }
    {
        double dl = (double)(t_v - t_dl) / 1000.0, vf = pal_ms_since(t_v);
        FILE *f = fopen(g.active, "w");
        if (f) {
            fprintf(f, "%s\n", version);
            fclose(f);
        }
        ota_ev("DEVICE", "DEVICE", "ACTIVATE", -1, "v%s -> v%s (simulated: no reboot, no flash)", cur, version);
        report("done", done, size, version);
        summary(image, version, size, true, "", dl, vf, pal_ms_since(t0), chunks, chunk, retries);
    }
    ctl_close();
    return 0;
}
