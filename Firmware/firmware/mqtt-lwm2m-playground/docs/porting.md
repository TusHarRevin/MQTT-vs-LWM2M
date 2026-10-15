# Porting to STM32 + EC200U

The port exists for NUCLEO-L452RE-P + EC200U-CN: see
[hardware/stm32l452-ec200u](../hardware/stm32l452-ec200u/README.md). It follows
the plan below. `pal.c` is replaced by `pal_ec200u.c` (AT sockets in direct
push mode). `events.c`/`control.c` are unchanged and reach the lab over the
ST-LINK serial port. netsim is kept, so impairment can be added on top of the
real network. This note explains how the code is split so a port replaces as
little as possible.

```
mqtt/main.c   lwm2m/main.c            application: sampling loop, benchmark
mqtt_client.c lwm2m_client.c objects.c  protocol logic       ── unchanged
bench.c sensor.c telemetry.c            portable C           ── unchanged
link.c                                  TLS/DTLS via mbedTLS  ── unchanged (bare-metal mbedTLS config)
netsim.c                                PC lab only           ── compile out
events.c control.c                      lab plumbing          ── redirect to a debug UART or drop
pal.c                                   OS: time, sockets     ── REPLACE
```

## The seam: `device/common/pal.h`

| PC (`pal.c` today) | STM32 + EC200U |
|---|---|
| `pal_now_us`, `pal_sleep_ms` | SysTick / a TIM, `HAL_Delay` or an RTOS delay |
| `pal_sock_connect(udp, host, port)` | `AT+QIOPEN=1,<id>,"TCP"\|"UDP","host",port` → `+QIOPEN: <id>,0` |
| `pal_sock_send` | `AT+QISEND=<id>,<len>` → `>` → data → `SEND OK` |
| `pal_sock_recv(timeout)` | wait for the `+QIURC: "recv",<id>` URC, then `AT+QIRD=<id>,<len>` |
| `pal_sock_close` | `AT+QICLOSE=<id>` |
| entropy for mbedTLS | the STM32 RNG peripheral |

Things to plan for:

- **UART framing:** a ring buffer filled by the UART RX interrupt/DMA, and an
  AT-response parser that also handles unsolicited result codes (`+QIURC`).
- **TLS placement:** keep mbedTLS on the STM32 (what this code does, and
  what lets you count every byte), or offload it to the modem's SSL stack
  (`AT+QSSLOPEN`). Offloading saves MCU flash and RAM but hides the handshake.
- **Memory:** the PC build is generous. Measure RAM and flash on the target with
  a trimmed `mbedtls_config` (only the cipher suites you use), and shrink
  `MQTT_BUF_SIZE` (20 KiB here) and the CoAP buffers to what your payloads need.
- **Time:** `lwm2m_gettime()` only needs monotonic seconds. Certificate
  validity checks need real time (RTC or network time from `AT+CCLK?`).
- **Power:** a modem in PSM/eDRX can't receive CoAP requests. That's what
  LwM2M queue mode (binding `UQ`) is for; it's not implemented here.
