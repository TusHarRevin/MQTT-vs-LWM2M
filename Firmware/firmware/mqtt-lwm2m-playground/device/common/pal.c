#include "pal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  include <direct.h>
#  include <io.h>
typedef int socklen_t;
#  define CLOSESOCK closesocket
#else
#  include <arpa/inet.h>
#  include <errno.h>
#  include <fcntl.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <signal.h>
#  include <sys/select.h>
#  include <sys/socket.h>
#  include <sys/stat.h>
#  include <unistd.h>
#  define CLOSESOCK close
#endif

/* ---- Time ---------------------------------------------------------------- */

uint64_t pal_now_us(void)
{
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (freq.QuadPart == 0) {
        QueryPerformanceFrequency(&freq);
    }
    QueryPerformanceCounter(&now);
    return (uint64_t)((now.QuadPart / freq.QuadPart) * 1000000ULL +
                      (now.QuadPart % freq.QuadPart) * 1000000ULL / freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
#endif
}

uint64_t pal_wall_ms(void)
{
#ifdef _WIN32
    FILETIME ft;
    ULARGE_INTEGER u;
    GetSystemTimePreciseAsFileTime(&ft);
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (u.QuadPart - 116444736000000000ULL) / 10000ULL;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
#endif
}

void pal_sleep_ms(uint32_t ms)
{
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec ts = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
#endif
}

void pal_format_clock(uint64_t wall_ms, char out[16])
{
    time_t secs = (time_t)(wall_ms / 1000);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &secs);
#else
    localtime_r(&secs, &tmv);
#endif
    snprintf(out, 16, "%02d:%02d:%02d.%03u", tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
             (unsigned)(wall_ms % 1000));
}

bool pal_file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f) {
        fclose(f);
        return true;
    }
    return false;
}

int pal_mkdir(const char *path)
{
#ifdef _WIN32
    return _mkdir(path) == 0 ? 0 : PAL_ERR;
#else
    return mkdir(path, 0755) == 0 ? 0 : PAL_ERR;
#endif
}

/* ---- Sockets ------------------------------------------------------------- */

/* Children started by the lab must not inherit our sockets. (On Windows the
 * lab spawns without handle inheritance instead.) */
static void no_inherit(pal_sock_t s)
{
#ifndef _WIN32
    fcntl((int)s, F_SETFD, FD_CLOEXEC);
#else
    (void)s;
#endif
}

int pal_net_init(void)
{
#ifdef _WIN32
    static bool done;
    WSADATA wsa;
    if (!done) {
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            return PAL_ERR;
        }
        done = true;
    }
#else
    signal(SIGPIPE, SIG_IGN);
#endif
    return 0;
}

int pal_sock_connect(pal_sock_t *out, bool udp, const char *host, uint16_t port)
{
    struct addrinfo hints, *res = NULL, *ai;
    char portstr[8];
    int rc = PAL_ERR;

    pal_net_init();
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = udp ? SOCK_DGRAM : SOCK_STREAM;
    snprintf(portstr, sizeof(portstr), "%u", (unsigned)port);
    if (getaddrinfo(host, portstr, &hints, &res) != 0) {
        return PAL_ERR;
    }
    for (ai = res; ai != NULL; ai = ai->ai_next) {
        pal_sock_t s = (pal_sock_t)socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == PAL_SOCK_INVALID) {
            continue;
        }
        no_inherit(s);
        if (connect((int)s, ai->ai_addr, (socklen_t)ai->ai_addrlen) == 0) {
            if (!udp) {
                int one = 1; /* MQTT packets are tiny; don't let Nagle hold them */
                setsockopt((int)s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
            }
            *out = s;
            rc = 0;
            break;
        }
        CLOSESOCK((int)s);
    }
    freeaddrinfo(res);
    return rc;
}

int pal_sock_send(pal_sock_t s, const void *data, size_t len)
{
    int n = (int)send((int)s, (const char *)data, (int)len, 0);
    return n < 0 ? PAL_ERR : n;
}

int pal_sock_send_all(pal_sock_t s, const void *data, size_t len)
{
    const char *p = data;
    while (len > 0) {
        int n = (int)send((int)s, p, (int)len, 0);
        if (n <= 0) {
            return PAL_ERR;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

int pal_sock_recv(pal_sock_t s, void *buf, size_t cap, uint32_t timeout_ms)
{
    fd_set rf;
    struct timeval tv;
    int n;

    FD_ZERO(&rf);
    FD_SET((int)s, &rf);
    tv.tv_sec = (long)(timeout_ms / 1000);
    tv.tv_usec = (long)(timeout_ms % 1000) * 1000;
    n = select((int)s + 1, &rf, NULL, NULL, &tv);
    if (n == 0) {
        return 0;
    }
    if (n < 0) {
        return PAL_ERR;
    }
    n = (int)recv((int)s, (char *)buf, (int)cap, 0);
    if (n == 0) {
        return PAL_CLOSED;
    }
    if (n < 0) {
#ifdef _WIN32
        /* A UDP send to a closed port comes back as WSAECONNRESET on the next
         * recv. Treat it like "nothing arrived" so CoAP can retransmit. */
        if (WSAGetLastError() == WSAECONNRESET || WSAGetLastError() == WSAEMSGSIZE) {
            return 0;
        }
#else
        if (errno == ECONNREFUSED) {
            return 0;
        }
#endif
        return PAL_ERR;
    }
    return n;
}

void pal_sock_close(pal_sock_t s)
{
    if (s != PAL_SOCK_INVALID) {
        CLOSESOCK((int)s);
    }
}

static int fill_addr(struct sockaddr_in *sa, const char *ip, uint16_t port)
{
    memset(sa, 0, sizeof(*sa));
    sa->sin_family = AF_INET;
    sa->sin_port = htons(port);
    return inet_pton(AF_INET, ip, &sa->sin_addr) == 1 ? 0 : PAL_ERR;
}

int pal_udp_bind(pal_sock_t *out, const char *ip, uint16_t port)
{
    struct sockaddr_in sa;
    pal_sock_t s;

    pal_net_init();
    if (fill_addr(&sa, ip, port) != 0) {
        return PAL_ERR;
    }
    s = (pal_sock_t)socket(AF_INET, SOCK_DGRAM, 0);
    if (s == PAL_SOCK_INVALID) {
        return PAL_ERR;
    }
    no_inherit(s);
    if (bind((int)s, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        CLOSESOCK((int)s);
        return PAL_ERR;
    }
    *out = s;
    return 0;
}

int pal_udp_sendto(pal_sock_t s, const char *ip, uint16_t port, const void *data, size_t len)
{
    struct sockaddr_in sa;
    if (fill_addr(&sa, ip, port) != 0) {
        return PAL_ERR;
    }
    return sendto((int)s, (const char *)data, (int)len, 0, (struct sockaddr *)&sa, sizeof(sa)) < 0
               ? PAL_ERR
               : 0;
}

int pal_tcp_listen(pal_sock_t *out, const char *ip, uint16_t port)
{
    struct sockaddr_in sa;
    pal_sock_t s;
    int one = 1;

    pal_net_init();
    if (fill_addr(&sa, ip, port) != 0) {
        return PAL_ERR;
    }
    s = (pal_sock_t)socket(AF_INET, SOCK_STREAM, 0);
    if (s == PAL_SOCK_INVALID) {
        return PAL_ERR;
    }
    no_inherit(s);
#ifndef _WIN32
    setsockopt((int)s, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one));
#else
    (void)one;
#endif
    if (bind((int)s, (struct sockaddr *)&sa, sizeof(sa)) != 0 || listen((int)s, 32) != 0) {
        CLOSESOCK((int)s);
        return PAL_ERR;
    }
    *out = s;
    return 0;
}

pal_sock_t pal_tcp_accept(pal_sock_t listener)
{
    pal_sock_t c = (pal_sock_t)accept((int)listener, NULL, NULL);
    if (c != PAL_SOCK_INVALID) {
        int one = 1;
        no_inherit(c);
        setsockopt((int)c, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
    }
    return c;
}

/* ---- PRNG ---------------------------------------------------------------- */

void pal_rng_seed(pal_rng_t *r, uint64_t seed)
{
    r->s = seed ? seed : 0x9E3779B97F4A7C15ULL;
}

uint32_t pal_rng_u32(pal_rng_t *r)
{
    uint64_t x = r->s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    r->s = x;
    return (uint32_t)((x * 0x2545F4914F6CDD1DULL) >> 32);
}

double pal_rng_unit(pal_rng_t *r)
{
    return pal_rng_u32(r) / 4294967296.0;
}

double pal_rng_gauss(pal_rng_t *r)
{
    /* Box-Muller; one value per call is plenty here. */
    double u1 = pal_rng_unit(r), u2 = pal_rng_unit(r);
    if (u1 < 1e-12) {
        u1 = 1e-12;
    }
    return sqrt(-2.0 * log(u1)) * cos(6.283185307179586 * u2);
}
