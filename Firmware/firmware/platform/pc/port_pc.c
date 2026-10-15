/* PC simulation port: Windows (Winsock) and POSIX (Linux/macOS). */

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <direct.h>
#else
#include <errno.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#endif

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/fw_log.h"
#include "platform/port.h"

#define STORAGE_DIR "fw_storage"

#ifdef _WIN32
typedef SOCKET sock_handle_t;
#define BAD_SOCK   INVALID_SOCKET
#define sock_close closesocket
#else
typedef int sock_handle_t;
#define BAD_SOCK   (-1)
#define sock_close close
#endif

static bool s_net_up;

/* ---- Lifecycle --------------------------------------------------------- */

int port_init(void)
{
    srand((unsigned)port_millis());
    return FW_OK;
}

/* ---- Time -------------------------------------------------------------- */

uint32_t port_millis(void)
{
#ifdef _WIN32
    return (uint32_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
#endif
}

void port_delay_ms(uint32_t ms)
{
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec ts = { (time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
#endif
}

/* ---- Power ------------------------------------------------------------- */

void port_sleep_ms(uint32_t ms)
{
    port_delay_ms(ms);
}

void port_reset(void)
{
    port_log(FW_LOG_WARN, "port", "reset requested: exiting (restart the process)");
    exit(EXIT_SUCCESS);
}

int port_battery_mv(void)
{
    /* Simulated cell: drains from 4100 mV to 3200 mV over 90 min, then refills. */
    uint32_t minutes = port_millis() / 60000u;
    return 4100 - (int)(minutes % 90u) * 10;
}

/* ---- Storage: one file per key ----------------------------------------- */

static void storage_path(char *out, size_t cap, const char *key)
{
    snprintf(out, cap, "%s/%s.bin", STORAGE_DIR, key);
}

int port_storage_read(const char *key, void *buf, size_t cap, size_t *out_len)
{
    char path[64];
    FILE *f;
    size_t n;
    int extra;

    storage_path(path, sizeof(path), key);
    f = fopen(path, "rb");
    if (f == NULL) {
        return FW_ERR_NOT_FOUND;
    }
    n = fread(buf, 1, cap, f);
    extra = fgetc(f);
    fclose(f);
    if (extra != EOF) {
        return FW_ERR_NOMEM;
    }
    *out_len = n;
    return FW_OK;
}

int port_storage_write(const char *key, const void *data, size_t len)
{
    char path[64], tmp[72];
    FILE *f;
    size_t n;

#ifdef _WIN32
    _mkdir(STORAGE_DIR);
#else
    mkdir(STORAGE_DIR, 0755);
#endif
    storage_path(path, sizeof(path), key);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    f = fopen(tmp, "wb");
    if (f == NULL) {
        return FW_ERR_IO;
    }
    n = fwrite(data, 1, len, f);
    if (fclose(f) != 0 || n != len) {
        remove(tmp);
        return FW_ERR_IO;
    }
#ifdef _WIN32
    remove(path); /* rename() does not overwrite on Windows */
#endif
    return rename(tmp, path) == 0 ? FW_OK : FW_ERR_IO;
}

int port_storage_erase(const char *key)
{
    char path[64];

    storage_path(path, sizeof(path), key);
    return remove(path) == 0 ? FW_OK : FW_ERR_NOT_FOUND;
}

/* ---- Network ----------------------------------------------------------- */

int port_net_init(void)
{
#ifdef _WIN32
    if (!s_net_up) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            return FW_ERR_IO;
        }
    }
#endif
    s_net_up = true;
    return FW_OK;
}

bool port_net_is_up(void)
{
    return s_net_up;
}

int port_sock_open(port_sock_t *out, port_sock_type_t type, const char *host, uint16_t port)
{
    struct addrinfo hints, *res = NULL, *ai;
    sock_handle_t fd = BAD_SOCK;
    char port_str[8];

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = type == PORT_SOCK_TCP ? SOCK_STREAM : SOCK_DGRAM;
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);

    if (getaddrinfo(host, port_str, &hints, &res) != 0) {
        return FW_ERR_NOT_FOUND;
    }
    for (ai = res; ai != NULL; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd == BAD_SOCK) {
            continue;
        }
        if (connect(fd, ai->ai_addr, (int)ai->ai_addrlen) == 0) {
            break;
        }
        sock_close(fd);
        fd = BAD_SOCK;
    }
    freeaddrinfo(res);
    if (fd == BAD_SOCK) {
        return FW_ERR_IO;
    }
    *out = (port_sock_t)fd;
    return FW_OK;
}

int port_sock_send(port_sock_t s, const uint8_t *data, size_t len)
{
    int rc = (int)send((sock_handle_t)s, (const char *)data, (int)len, 0);

    return rc >= 0 ? rc : FW_ERR_IO;
}

int port_sock_recv(port_sock_t s, uint8_t *buf, size_t cap, uint32_t timeout_ms)
{
    sock_handle_t fd = (sock_handle_t)s;
    struct timeval tv, *ptv = NULL;
    fd_set rfds;
    int rc;

    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    if (timeout_ms != FW_WAIT_FOREVER) {
        tv.tv_sec = (long)(timeout_ms / 1000u);
        tv.tv_usec = (long)(timeout_ms % 1000u) * 1000L;
        ptv = &tv;
    }
    rc = select((int)(fd + 1), &rfds, NULL, NULL, ptv);
    if (rc == 0) {
        return FW_ERR_TIMEOUT;
    }
    if (rc < 0) {
#ifndef _WIN32
        if (errno == EINTR) {
            return FW_ERR_TIMEOUT;
        }
#endif
        return FW_ERR_IO;
    }
    rc = (int)recv(fd, (char *)buf, (int)cap, 0);
    if (rc > 0) {
        return rc;
    }
    return rc == 0 ? FW_ERR_CLOSED : FW_ERR_IO;
}

void port_sock_close(port_sock_t s)
{
    sock_close((sock_handle_t)s);
}

/* ---- Sensors: simulated ------------------------------------------------ */

static float noise(float amplitude)
{
    return amplitude * ((float)rand() / (float)RAND_MAX * 2.0f - 1.0f);
}

int port_sensor_read(unsigned channel, float *value)
{
    float t = (float)port_millis() / 1000.0f;

    switch (channel) {
    case 0: /* temperature, Cel */
        *value = 22.0f + 3.0f * sinf(t / 60.0f) + noise(0.2f);
        return FW_OK;
    case 1: /* relative humidity, % */
        *value = 45.0f + 10.0f * sinf(t / 300.0f) + noise(1.0f);
        return FW_OK;
    default:
        return FW_ERR_NOT_FOUND;
    }
}

/* ---- Entropy ----------------------------------------------------------- */

int port_entropy(uint8_t *buf, size_t len)
{
#ifdef _WIN32
    return BCryptGenRandom(NULL, buf, (ULONG)len, BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0
               ? FW_OK
               : FW_ERR_IO;
#else
    FILE *f = fopen("/dev/urandom", "rb");
    size_t n;

    if (f == NULL) {
        return FW_ERR_IO;
    }
    n = fread(buf, 1, len, f);
    fclose(f);
    return n == len ? FW_OK : FW_ERR_IO;
#endif
}

/* ---- Logging ----------------------------------------------------------- */

void port_log(int level, const char *tag, const char *fmt, ...)
{
    static const char levels[] = "EWID";
    va_list ap;

    printf("%10lu %c [%s] ", (unsigned long)port_millis(),
           (level >= 0 && level <= 3) ? levels[level] : '?', tag);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
}
