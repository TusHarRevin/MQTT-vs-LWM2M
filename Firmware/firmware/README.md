# Firmware

Portable IoT device firmware. It sends sensor telemetry over **MQTT** (Paho
Embedded-C) or **LwM2M** (Eclipse Wakaama), secured with **TLS/DTLS**
(mbedTLS). It runs as a simulation on a PC and is ported to STM32.

## Layers

Each layer only calls the layer directly below it. Only `platform/` touches
hardware or the OS.

```
application/   device.c (state machine, commands) · telemetry.c (JSON frames) · sensor.c (filtering)
     │
protocol/      protocol_manager.c (backend select, reconnect/backoff)
     │           ├─ mqtt_client.c   (Paho MQTTPacket, QoS 0/1, keep-alive)
     │           └─ lwm2m_client.c  (Wakaama glue + connection callbacks)
     │
security/      tls.c (TCP) · dtls.c (UDP, PSK or CA) · tls_common.c (DRBG, errors)
     │
network/       network.c (link + fw_transport_t interface) · socket.c
     │
hal/           timer.c (wrap-safe deadlines) · storage.c (CRC-checked KV) · power.c
     │
platform/      port.h (the contract) → pc/port_pc.c · stm32/port_stm32.c
```

`fw_transport_t` (in `network/network.h`) lets the protocol clients run
unchanged over a plain socket, TLS or DTLS.

## Build: PC

Requires CMake ≥ 3.20, a C11 compiler and git (the libraries are fetched at
configure time).

```sh
cmake -B build
cmake --build build
./build/firmware          # Windows: build\Debug\firmware.exe (MSVC) or build\firmware.exe
```

By default it connects to `test.mosquitto.org:1883` over plain TCP and publishes
to `devices/<id>/telemetry` every 10 s. To watch:

```sh
mosquitto_sub -h test.mosquitto.org -t 'devices/+/telemetry' -v
mosquitto_pub -h test.mosquitto.org -t 'devices/<id>/cmd' -m 'interval=5000'
```

Persistent data (device ID, interval) is stored in `./fw_storage/`.

### Options

| CMake option | Default | |
|---|---|---|
| `FW_PLATFORM` | `pc` | `pc` or `stm32` |
| `FW_USE_MBEDTLS` / `FW_USE_PAHO` / `FW_USE_WAKAAMA` | `ON` | When a library is `OFF`, its module still compiles and returns `FW_ERR_UNSUPPORTED` |

Runtime settings live in `common/fw_config.h`. Each one can be overridden with
`-D`, for example:

```sh
cmake -B build -DCMAKE_C_FLAGS="-DFW_MQTT_USE_TLS=1 -DFW_PROTOCOL=FW_PROTOCOL_MQTT"
```

TLS requires a CA (`FW_MQTT_CA_PEM`). It refuses to connect without one unless
`FW_TLS_ALLOW_INSECURE=1`, which is for development only.

## Build: STM32

`firmware_stm32` is a static library that you link into a STM32CubeMX project.
See the comment block in `CMakeLists.txt`. The Cube project needs:

- **RNG**, a **log UART** (`huart2` by default) and **LwIP** with
  `LWIP_SOCKET=1`, `LWIP_DNS=1` and an RTOS (`NO_SYS=0`)
- a call to `device_init()` once, then `device_step()` in a loop, from a task
- `-u _printf_float` in the linker flags if you use newlib-nano (the telemetry
  JSON uses `%f`)

mbedTLS is configured for bare metal by `platform/stm32/mbedtls_user_config.h`.

## What is still a stub

| Where | What's missing |
|---|---|
| `platform/stm32/port_stm32.c` | Flash storage, battery ADC, real sensors, STOP-mode sleep |
| `platform/stm32/mbedtls_user_config.h` | No RTC, so certificate validity dates are **not** checked |
| `application/device.c` | LwM2M objects (Security, Server, Device, IPSO 3303/3304) and the DTLS PSK |
| `protocol/lwm2m_client.c` | Server URI comes from config, not the Security object (no bootstrap) |
| `cmake/deps.cmake` | Wakaama isn't pinned to a commit yet |

LwM2M mode returns `FW_ERR_PARAM` until the objects are supplied. Wakaama's
`examples/client/object_*.c` are the usual starting point.
