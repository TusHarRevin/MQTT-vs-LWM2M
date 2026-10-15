/*
 * STM32 port: STM32Cube HAL + LwIP socket API.
 *
 * Assumes a CubeMX-generated project:
 *   - "main.h" pulls in the stm32xxxx_hal.h for your part
 *   - RNG, a log UART and LwIP (with LWIP_SOCKET=1, LWIP_DNS=1) are enabled
 *   - the LwIP socket API needs an RTOS (NO_SYS=0), e.g. FreeRTOS from CubeMX
 *
 * Handle names below match CubeMX defaults; override with -D if yours differ.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "main.h"

#include "lwip/netdb.h"
#include "lwip/netif.h"
#include "lwip/sockets.h"

#include "common/fw_log.h"
#include "platform/port.h"

#ifndef FW_STM32_RNG
#define FW_STM32_RNG hrng
#endif
#ifndef FW_STM32_LOG_UART
#define FW_STM32_LOG_UART huart2
#endif

extern RNG_HandleTypeDef FW_STM32_RNG;
extern UART_HandleTypeDef FW_STM32_LOG_UART;

/* ---- Lifecycle --------------------------------------------------------- */

int port_init(void)
{
    /* HAL_Init(), clocks, peripherals and MX_LWIP_Init() are done by CubeMX
     * before device_init() is called. */
    return FW_OK;
}

/* ---- Time -------------------------------------------------------------- */

uint32_t port_millis(void)
{
    return HAL_GetTick();
}

void port_delay_ms(uint32_t ms)
{
    /* Under FreeRTOS prefer vTaskDelay(pdMS_TO_TICKS(ms)) so other tasks run. */
    HAL_Delay(ms);
}

/* ---- Power ------------------------------------------------------------- */

void port_sleep_ms(uint32_t ms)
{
    /* Sleep mode between SysTick interrupts.
     * TODO: STOP mode + RTC wake-up for long idle periods. */
    uint32_t start = HAL_GetTick();

    while ((uint32_t)(HAL_GetTick() - start) < ms) {
        __WFI();
    }
}

void port_reset(void)
{
    NVIC_SystemReset();
}

int port_battery_mv(void)
{
    /* TODO: read VBAT or a divider via ADC (VREFINT-calibrated). */
    return -1;
}

/* ---- Storage ----------------------------------------------------------- */

/* TODO: back this with internal flash (e.g. ST's EEPROM emulation, or a
 * reserved sector with a simple log-structured key store) or external
 * SPI flash. Until then nothing persists: the device ID is regenerated on
 * every boot. */
int port_storage_read(const char *key, void *buf, size_t cap, size_t *out_len)
{
    FW_UNUSED(key); FW_UNUSED(buf); FW_UNUSED(cap); FW_UNUSED(out_len);
    return FW_ERR_NOT_FOUND;
}

int port_storage_write(const char *key, const void *data, size_t len)
{
    FW_UNUSED(key); FW_UNUSED(data); FW_UNUSED(len);
    return FW_ERR_UNSUPPORTED;
}

int port_storage_erase(const char *key)
{
    FW_UNUSED(key);
    return FW_ERR_UNSUPPORTED;
}

/* ---- Network (LwIP sockets) -------------------------------------------- */

int port_net_init(void)
{
    return FW_OK; /* MX_LWIP_Init() already ran; DHCP completes asynchronously */
}

bool port_net_is_up(void)
{
    struct netif *nif = netif_default;

    return nif != NULL && netif_is_up(nif) && netif_is_link_up(nif) &&
           !ip_addr_isany(&nif->ip_addr);
}

int port_sock_open(port_sock_t *out, port_sock_type_t type, const char *host, uint16_t port)
{
    struct addrinfo hints, *res = NULL, *ai;
    char port_str[8];
    int fd = -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = type == PORT_SOCK_TCP ? SOCK_STREAM : SOCK_DGRAM;
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);

    if (lwip_getaddrinfo(host, port_str, &hints, &res) != 0) {
        return FW_ERR_NOT_FOUND;
    }
    for (ai = res; ai != NULL; ai = ai->ai_next) {
        fd = lwip_socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (lwip_connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            break;
        }
        lwip_close(fd);
        fd = -1;
    }
    lwip_freeaddrinfo(res);
    if (fd < 0) {
        return FW_ERR_IO;
    }
    *out = (port_sock_t)fd;
    return FW_OK;
}

int port_sock_send(port_sock_t s, const uint8_t *data, size_t len)
{
    int rc = (int)lwip_send((int)s, data, len, 0);

    return rc >= 0 ? rc : FW_ERR_IO;
}

int port_sock_recv(port_sock_t s, uint8_t *buf, size_t cap, uint32_t timeout_ms)
{
    int fd = (int)s;
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
    rc = lwip_select(fd + 1, &rfds, NULL, NULL, ptv);
    if (rc == 0) {
        return FW_ERR_TIMEOUT;
    }
    if (rc < 0) {
        return FW_ERR_IO;
    }
    rc = (int)lwip_recv(fd, buf, cap, 0);
    if (rc > 0) {
        return rc;
    }
    return rc == 0 ? FW_ERR_CLOSED : FW_ERR_IO;
}

void port_sock_close(port_sock_t s)
{
    lwip_close((int)s);
}

/* ---- Sensors ----------------------------------------------------------- */

int port_sensor_read(unsigned channel, float *value)
{
    /* TODO: map channels to real sensors, e.g. 0 = temperature and
     * 1 = humidity from an SHT3x/HDC1080 over I2C (HAL_I2C_Master_Receive). */
    FW_UNUSED(channel); FW_UNUSED(value);
    return FW_ERR_UNSUPPORTED;
}

/* ---- Entropy ----------------------------------------------------------- */

int port_entropy(uint8_t *buf, size_t len)
{
    while (len > 0) {
        uint32_t word;
        size_t n = len < sizeof(word) ? len : sizeof(word);

        if (HAL_RNG_GenerateRandomNumber(&FW_STM32_RNG, &word) != HAL_OK) {
            return FW_ERR_IO;
        }
        memcpy(buf, &word, n);
        buf += n;
        len -= n;
    }
    return FW_OK;
}

#if FW_HAVE_MBEDTLS
/* Entropy source for mbedTLS, enabled by MBEDTLS_ENTROPY_HARDWARE_ALT in
 * mbedtls_user_config.h. */
int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen)
{
    FW_UNUSED(data);
    if (port_entropy(output, len) != FW_OK) {
        *olen = 0;
        return -1;
    }
    *olen = len;
    return 0;
}
#endif

/* ---- Logging ----------------------------------------------------------- */

void port_log(int level, const char *tag, const char *fmt, ...)
{
    static const char levels[] = "EWID";
    char buf[192];
    va_list ap;
    int n;

    n = snprintf(buf, sizeof(buf), "%10lu %c [%s] ", (unsigned long)HAL_GetTick(),
                 (level >= 0 && level <= 3) ? levels[level] : '?', tag);
    if (n < 0) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(buf + n, sizeof(buf) - (size_t)n, fmt, ap);
    va_end(ap);

    n = (int)strlen(buf);
    if (n > (int)sizeof(buf) - 3) {
        n = (int)sizeof(buf) - 3;
    }
    buf[n++] = '\r';
    buf[n++] = '\n';
    HAL_UART_Transmit(&FW_STM32_LOG_UART, (uint8_t *)buf, (uint16_t)n, 100);
}
