#include "serial.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>

struct serial
{
    HANDLE h;
    uint32_t timeout_ms;
};

static void set_timeout(serial_t *s, uint32_t ms)
{
    COMMTIMEOUTS to = { 0 };
    if (s->timeout_ms == ms) {
        return;
    }
    /* return as soon as anything arrives, or after ms */
    to.ReadIntervalTimeout = MAXDWORD;
    to.ReadTotalTimeoutMultiplier = MAXDWORD;
    to.ReadTotalTimeoutConstant = ms ? ms : 1;
    to.WriteTotalTimeoutConstant = 2000;
    SetCommTimeouts(s->h, &to);
    s->timeout_ms = ms;
}

serial_t *serial_open(const char *port, unsigned baud)
{
    char path[64];
    DCB dcb;
    serial_t *s;
    HANDLE h;

    snprintf(path, sizeof(path), "\\\\.\\%s", port); /* needed for COM10 and up */
    h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return NULL;
    }
    memset(&dcb, 0, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);
    GetCommState(h, &dcb);
    dcb.BaudRate = baud;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    if (!SetCommState(h, &dcb)) {
        CloseHandle(h);
        return NULL;
    }
    SetupComm(h, 65536, 8192);
    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
    s = calloc(1, sizeof(*s));
    if (s == NULL) {
        CloseHandle(h);
        return NULL;
    }
    s->h = h;
    s->timeout_ms = (uint32_t)-1;
    return s;
}

int serial_read(serial_t *s, void *buf, size_t cap, uint32_t timeout_ms)
{
    DWORD n = 0;
    /* Synchronous handles serialise reads and writes, so keep each read
     * short: a command written meanwhile waits at most this long. */
    set_timeout(s, timeout_ms > 50 ? 50 : timeout_ms);
    if (!ReadFile(s->h, buf, (DWORD)cap, &n, NULL)) {
        return -1;
    }
    return (int)n;
}

int serial_write(serial_t *s, const void *data, size_t len)
{
    DWORD n = 0;
    if (!WriteFile(s->h, data, (DWORD)len, &n, NULL) || n != len) {
        return -1;
    }
    return 0;
}

void serial_close(serial_t *s)
{
    if (s) {
        CloseHandle(s->h);
        free(s);
    }
}

#else /* POSIX */
#  include <errno.h>
#  include <fcntl.h>
#  include <sys/select.h>
#  include <termios.h>
#  include <unistd.h>

struct serial
{
    int fd;
};

static speed_t to_speed(unsigned baud)
{
    switch (baud) {
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
    case 57600: return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
#  ifdef B460800
    case 460800: return B460800;
#  endif
#  ifdef B921600
    case 921600: return B921600;
#  endif
    default: return B115200;
    }
}

serial_t *serial_open(const char *port, unsigned baud)
{
    struct termios t;
    serial_t *s;
    int fd = open(port, O_RDWR | O_NOCTTY | O_CLOEXEC);

    if (fd < 0) {
        return NULL;
    }
    if (tcgetattr(fd, &t) != 0) {
        close(fd);
        return NULL;
    }
    cfmakeraw(&t);
    t.c_cflag |= CLOCAL | CREAD;
    t.c_cflag &= ~(CSTOPB | CRTSCTS);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    cfsetispeed(&t, to_speed(baud));
    cfsetospeed(&t, to_speed(baud));
    if (tcsetattr(fd, TCSANOW, &t) != 0) {
        close(fd);
        return NULL;
    }
    tcflush(fd, TCIOFLUSH);
    s = calloc(1, sizeof(*s));
    if (s == NULL) {
        close(fd);
        return NULL;
    }
    s->fd = fd;
    return s;
}

int serial_read(serial_t *s, void *buf, size_t cap, uint32_t timeout_ms)
{
    fd_set rf;
    struct timeval tv = { (time_t)(timeout_ms / 1000), (suseconds_t)(timeout_ms % 1000) * 1000 };
    int n;

    FD_ZERO(&rf);
    FD_SET(s->fd, &rf);
    n = select(s->fd + 1, &rf, NULL, NULL, &tv);
    if (n == 0 || (n < 0 && errno == EINTR)) {
        return 0;
    }
    if (n < 0) {
        return -1;
    }
    n = (int)read(s->fd, buf, cap);
    return n > 0 ? n : -1; /* readable but 0 bytes = device gone */
}

int serial_write(serial_t *s, const void *data, size_t len)
{
    const char *p = data;
    while (len > 0) {
        ssize_t n = write(s->fd, p, len);
        if (n <= 0) {
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

void serial_close(serial_t *s)
{
    if (s) {
        close(s->fd);
        free(s);
    }
}
#endif
