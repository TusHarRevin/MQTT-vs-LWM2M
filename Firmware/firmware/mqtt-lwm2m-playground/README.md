# MQTT vs LwM2M Playground

## What this project is

A small, hands-on **embedded IoT protocol laboratory that runs entirely on your PC**.
No STM32 board or modem needed.

```
                      ┌──────────── MQTT / TCP (TLS) ─────────────► Mosquitto
 Embedded-C simulated │
 sensor device (C)  ──┤
                      └──── LwM2M / CoAP / UDP (DTLS) ────────────► Leshan
          │
          └── events ──► lab (C web server) ──► browser dashboard
```

- `mqtt_device` and `lwm2m_device` are **C programs** using the same libraries
  you would put on a microcontroller: Eclipse **Paho** MQTTPacket, Eclipse
  **Wakaama** (LwM2M), **mbedTLS** (TLS/DTLS).
- They talk to **real servers**: Eclipse **Mosquitto** and Eclipse **Leshan**.
- `lab` is a small C web server that starts the devices, watches both servers
  and streams every protocol message to an interactive dashboard.
- No Python needed anywhere. The dashboard is HTML + JavaScript with
  no external libraries, so it works offline.

## What you will learn

- The MQTT handshake and QoS 0/1/2 flows:
  `CONNECT → CONNACK → PUBLISH → PUBACK` and `PUBREC/PUBREL/PUBCOMP`.
- LwM2M over CoAP: `REGISTER → 2.01`, `READ/WRITE/EXECUTE`, `OBSERVE → NOTIFY → ACK`,
  and the standard objects (`/3/0/9` battery, `/3303/0/5700` temperature, …).
- What packet loss and latency do to each protocol: TCP retransmission versus CoAP's
  own retransmission.
- What TLS (MQTT) and DTLS-PSK (LwM2M) cost in handshake time and bytes.
- How to benchmark without fooling yourself: min/avg/median/P95/P99, with no
  invented numbers.
- A simulated OTA: manifest, chunked download, storage, SHA-256 verification, activation.

## What you need installed

| | Windows | Linux / macOS / WSL |
|---|---|---|
| C compiler + CMake ≥ 3.20 | MinGW-w64 gcc or Visual Studio, CMake, Ninja | `gcc`/`clang`, `cmake`, `ninja` or `make` |
| git (first build downloads mbedTLS, Paho, Wakaama) | ✓ | ✓ |
| **Servers: either** Docker | Docker Desktop | Docker |
| **or** without Docker | `scripts\get-servers.ps1` (portable Mosquitto + Java + Leshan, no admin) | `scripts/get-servers.sh` (apt/brew mosquitto + Leshan jar; Java 17+) |
| A browser | ✓ | ✓ |

## How to build

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build        # optional: 5 unit tests
```

This produces `build/mqtt_device`, `build/lwm2m_device`, `build/ota_device` and `build/lab`
(`.exe` on Windows), plus the sample images `build/ota/firmware_v1.bin` and `firmware_v2.bin`.

## Start the servers

With Docker:

```bash
docker compose up
```

Without Docker (Windows PowerShell, two windows):

```powershell
powershell -ExecutionPolicy Bypass -File scripts\get-servers.ps1     # once
powershell -ExecutionPolicy Bypass -File scripts\start-mosquitto.ps1
powershell -ExecutionPolicy Bypass -File scripts\start-leshan.ps1
```

Without Docker (Linux / macOS / WSL):

```bash
scripts/get-servers.sh        # once
scripts/start-mosquitto.sh    # terminal 1
scripts/start-leshan.sh       # terminal 2
```

> **Windows: "access forbidden" on port 1883?** Hyper-V/WSL often reserve a
> port range that includes 1883. See
> [troubleshooting](docs/troubleshooting.md#port-1883-is-forbidden-on-windows).
> Quick workaround: `start-mosquitto.ps1 -Port 21883 -TlsPort 28883` and
> `build\lab --mqtt-port 21883 --mqtts-port 28883`.

## How to start MQTT

```bash
./build/mqtt_device                                  # QoS 1, every 10 s, until Ctrl+C
./build/mqtt_device --qos 2 --interval 1s --count 20
./build/mqtt_device --secure --ca servers/mosquitto/certs/ca.crt --tls-name localhost
./build/mqtt_device --help
```

Watch it from another terminal with `mosquitto_sub -t 'playground/#' -v`.

## How to start LwM2M

```bash
./build/lwm2m_device                                 # registers with Leshan at 127.0.0.1:5683
```

Open http://127.0.0.1:8080, click the client **lwm2m-dev-1** and press
*Observe* on Temperature (`/3303/0/5700`). Notifications start every interval.
For DTLS, see [docs/security.md](docs/security.md).

## How to open the dashboard

```bash
./build/lab            # then open http://127.0.0.1:8000
```

The lab finds the device programs next to itself and the dashboard files in
`dashboard/www`. Use `./build/lab --help` for ports.

## How to run the first experiment (≈ 5 minutes)

1. Servers running, `./build/lab` running, browser on http://127.0.0.1:8000.
   Both pills in the header say **ONLINE**.
2. **MQTT tab → START.** Arrows appear: `CONNECT`, `CONNACK`, `SUBSCRIBE`,
   `SUBACK`, then `PUBLISH`/`PUBACK` once a second. Click any event in the log
   to see its raw bytes in the inspector.
3. **LwM2M tab → START.** `REGISTER → REGISTER RESPONSE (2.01)`. The dashboard
   then OBSERVEs temperature, and `NOTIFY → ACK` follows every interval.
   Select `/3303/0/5750`, type a value next to *Write*, and press **Write**.
4. **MQTT vs LwM2M tab → "Benchmark: 100 + 100".** It sends 100 MQTT
   messages, then 100 LwM2M notifications. The table fills with measured
   numbers.
5. Set **Packet loss = 5 %** on either tab, then press **Sweep packet loss**,
   or **Security OFF vs ON**. The charts update after each run.
6. **OTA tab → START OTA**, then try **INTERRUPT OTA** on the *Poor* network profile.

Every completed run is appended to `results/runs.jsonl`, and the per-message
data goes to `results/<run>.csv`.

### Example (measured on the development PC, Windows 11, loopback, no impairment)

```
==== MQTT run (QoS 1, 100 messages) ====
messages 100  ok 100  failed 0  success 100.0 %
latency ms  min 0.10  avg 0.50  median 0.41  p95 0.86  p99 1.63  max 3.42

==== LwM2M run (CON notifications, 100) ====
notifications 100  ok 100  failed 0  success 100.0 %
latency ms (CON->ACK)  min 0.86  avg 2.41  median 1.81  p95 6.00  p99 8.26  max 16.13
```

Your numbers will differ. That's the point: measure on your own machine.

## Project layout

```
device/common/   sensor.c telemetry.c   simulated sensor + JSON payload
                 link.c netsim.c pal.c  transport: socket -> netsim -> TLS/DTLS (PAL = OS seam)
                 events.c control.c cli.c
device/mqtt/     main.c mqtt_client.c   MQTT 3.1.1 client on Paho MQTTPacket
device/lwm2m/    main.c lwm2m_client.c  Wakaama glue, CON notifications, CoAP tracing
                 objects.c              /0 /1 /3 /19 /3303 /3304 /3315
benchmark/       bench.c                per-message records, percentiles, CSV
ota/             ota_device.c make_firmware.c   SIMULATED OTA
dashboard/       server/ (lab, C)  www/ (HTML/JS/CSS)
servers/         mosquitto/ (config, lab TLS certs)  leshan/ (Dockerfile)
scripts/         get/start servers, netem.sh
tests/           unit tests (ctest)
docs/            mqtt, lwm2m, security, ota, benchmarking, server, troubleshooting, porting
```

## Running on real hardware: STM32L452 + EC200U

The same MQTT and LwM2M device code also runs on a NUCLEO-L452RE-P with a
Quectel EC200U over LTE, driven from the dashboard (**Device: STM32 + EC200U**
in the header). See [hardware/stm32l452-ec200u/README.md](hardware/stm32l452-ec200u/README.md)
for wiring, flashing and the `--hw-*` lab options. Only `device/common/pal.c` is
replaced: see [docs/porting.md](docs/porting.md).

## Free public servers instead of local ones

`.\build\lab.exe --public` uses test.mosquitto.org and leshan.eclipseprojects.io
instead of a local Mosquitto and Leshan. You start nothing else, and a device on
a cellular network (the STM32 board) can reach them too. Your devices get a
unique name prefix (`pg-xxxxxx`), and the dashboard shows only those. The data
is visible to anyone on those servers.

## Honest limits

- The network simulator is application-level (see
  [benchmarking.md](docs/benchmarking.md#network-simulation)). Use `tc netem` on
  Linux/WSL for packet-accurate impairment.
- LwM2M notifications are sent **confirmable** so they can be acknowledged and
  timed. Stock Wakaama sends them non-confirmable; `--notify-non` restores that.
- OTA is a **simulation**: no flash, bootloader or rollback.
- The TLS certificates in `servers/mosquitto/certs` and the default DTLS key are
  for this lab only.
