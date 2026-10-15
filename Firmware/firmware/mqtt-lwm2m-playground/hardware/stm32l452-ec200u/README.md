# Real hardware: NUCLEO-L452RE-P + Quectel EC200U-CN

The playground's `mqtt_device` and `lwm2m_device` running on a real board over LTE.
The protocol code is the same: `device/mqtt`, `device/lwm2m`, `link.c` (mbedTLS), Paho,
Wakaama. Only the OS layer differs: `src/pal_ec200u.c` implements `device/common/pal.h`
with EC200U AT sockets instead of PC sockets.

```
 dashboard ── lab.exe ──USB (ST-LINK serial, 921600)── STM32L452 ──UART 115200── EC200U ── LTE ──┐
               │  "run mqtt --qos 1 ..." / events as JSON lines                                     │
               │                                                                                    ▼
               └──── watches ────►  test.mosquitto.org (MQTT)  ·  leshan.eclipseprojects.io (LwM2M)
```

With `lab.exe --public`, the board, the PC devices and the lab all use the **free public test
servers** of the Eclipse project, so you need no tunnel and no router setup. The dashboard works
as usual: pick **Device: STM32 + EC200U** in the header. (To use your local Mosquitto and Leshan
instead, see [Alternative: your local servers](#alternative-your-local-servers).)

## Wiring

| NUCLEO-L452RE-P | EC200U board | |
|---|---|---|
| PA9 (D8) USART1_TX | RXD | **the EC200U UART is 1.8 V: use a level shifter** unless your board has one |
| PA10 (D2) USART1_RX | TXD | |
| PA0 (A0) | PWRKEY | optional; pulsed low for 1.25 s only if the modem doesn't answer `AT` |
| GND | GND | |
| USB (ST-LINK) | | power, flashing, and the link to the lab (USART2 PA2/PA3) |

Power the EC200U from its own supply (up to 2 A peaks). This is the same wiring as
`R:\Revin_ECO\EC200`. Change pins, APN and baud rates in `src/hw_config.h`.

**APN:** the default is `airtelgprs.com`. For Jio, build with
`-DHW_DEFINES="MODEM_APN=\"jionet\""`, or type `apn jionet` in a serial terminal. That setting
lasts until the next power-off.

## Build and flash

You need STM32CubeCLT (compiler, CMake, Ninja, programmer: `R:\STM_driver_VS\STM32CubeCLT_1.22.0`)
and STM32CubeL4 1.18.2 (`%USERPROFILE%\STM32Cube\Repository`, installed by CubeIDE/CubeMX).
Both are already on this PC. Run from the playground folder:

```powershell
cd "R:\Revin\Firmware\firmware\mqtt-lwm2m-playground"
$clt = "R:\STM_driver_VS\STM32CubeCLT_1.22.0"
$hw  = "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/hardware/stm32l452-ec200u"
& "$clt\CMake\bin\cmake.exe" -S $hw -B build-stm32 -G Ninja "-DCMAKE_MAKE_PROGRAM=$clt/Ninja/bin/ninja.exe" "-DCMAKE_TOOLCHAIN_FILE=$hw/cmake/arm-none-eabi.cmake"
& "$clt\CMake\bin\cmake.exe" --build build-stm32
& "$clt\CMake\bin\cmake.exe" --build build-stm32 --target flash
```

(Quote the `-D...` arguments in PowerShell.) The libraries come from the PC build's
`build\_src` (or `build\_deps`) folder, so no git or network access is needed. The output is
`build-stm32\pg_firmware.elf`, `.hex` and `.bin`. Currently it uses about 290 KB of flash and
80 KB of static RAM; the rest of the RAM goes to the heap and the stack.

After flashing, the green LED (LD4) lights when the modem has a data connection. A fast blink
means the modem bring-up failed. Open a terminal on the ST-LINK COM port at 921600 baud to see
why.

## Run (free public test servers)

You don't need to start Mosquitto or Leshan. One command does everything:

```powershell
cd "R:\Revin\Firmware\firmware\mqtt-lwm2m-playground"
.\build\lab.exe --public --hw-serial COM10
```

1. Close any serial terminal first: only one program can open COM10 at a time.
2. Open http://127.0.0.1:8000 and select **Device: STM32 + EC200U** in the header. The
   **Board** pill turns green (READY) once the modem has an IP address. Hover over it to see
   the operator, signal and IP.
3. Use the MQTT, LwM2M and "MQTT vs LwM2M" tabs exactly as with the PC devices. With
   **Device: PC simulation**, the PC devices use the same public servers, so PC and board
   results are directly comparable.

What `--public` sets up:

| | Server | Board / PC connect to |
|---|---|---|
| MQTT | test.mosquitto.org | 1883 plain, 8883 TLS (verified against `servers/public/mosquitto.org.crt`, built into the firmware) |
| LwM2M | leshan.eclipseprojects.io | 5683 CoAP, 5684 DTLS-PSK (the lab registers the PSK through the sandbox's HTTPS API) |

- **Everything is public.** Device names and topics are prefixed with a code derived from this
  PC's name, e.g. `pg-c01d8b-stm32-mqtt` and `playground/pg-c01d8b/...`. That keeps them apart
  from other users, and the dashboard shows only yours. Anyone on those servers can still see
  them. Change the prefix with `--id-prefix`.
- The servers are shared and come with no guarantee. Occasionally one drops or refuses a
  connection; the log then shows e.g. `no CONNACK (connection lost: server closed the
  connection)`. Try again.
- The Leshan sandbox web page, https://leshan.eclipseprojects.io, shows your endpoint too.

## Alternative: your local servers

The board can't reach `127.0.0.1` from the cellular network. To use your own Mosquitto and
Leshan, make them reachable from the Internet in one of two ways:

- **router port forwarding** (free, needs a public IPv4 address)
- **a tunnel that supports TCP and UDP** (usually paid)

| Local port | Type | Used for | lab option |
|---|---|---|---|
| 1883 (or 21883) | TCP | MQTT, security OFF | `--hw-mqtt host:port` |
| 8883 (or 28883) | TCP | MQTT, security ON | `--hw-mqtts host:port` |
| 5683 | UDP | LwM2M, security OFF | `--hw-coap host:port` |
| 5684 | UDP | LwM2M, security ON | `--hw-coaps host:port` |

```powershell
.\build\lab.exe --mqtt-port 21883 --mqtts-port 28883 --hw-serial COM10 `
  --hw-mqtt <public-host>:<port> --hw-mqtts <public-host>:<port> `
  --hw-coap <public-host>:<port> --hw-coaps <public-host>:<port>
```

### What the lab sends to the board

The serial link is plain text, one command per line, so you can also drive the board by hand
from a terminal (921600 baud):

```
time 1791214374000 330                 wall clock (ms) + UTC offset (min); the lab does this
run mqtt --host test.mosquitto.org --port 1883 --id pg-test-1 --topic playground/pg-test --qos 1 --interval 1s --count 20
run mqtt --host test.mosquitto.org --port 8883 --secure --ca mosquitto.org.crt --id pg-test-1 --count 5
run lwm2m --host leshan.eclipseprojects.io --port 5683 --id pg-test-2 --interval 2s
status                                 modem state as JSON
at AT+CSQ                              any AT command
apn jionet                             change APN and reconnect
reset
```

During a run, lines are device commands (`stop`, `pause`, `resume`, `publish`, `notify`,
`interval=2s`, ...). Output lines that start with `{` are JSON events for the lab. All other
lines are the same console output the PC devices print. After each run the board resets
itself, so every run starts from a clean state.

## Interpreting the numbers

- **Latency is real end-to-end time**: STM32 → UART → EC200U → LTE → Internet → the server
  → back. The public servers are in Europe, so expect a few hundred ms. From this PC the
  round trip to them is about 165 ms; LTE adds its own delay on top. Compare MQTT and LwM2M
  under the same conditions; don't read the numbers as absolute figures for a deployment.
- The EC200U UART runs at 115200 baud. A 1 KB packet spends about 90 ms on that wire, and
  this time is included in the latency. It is a real cost of an AT-command modem.
- The handshake times include the Cortex-M4 doing the crypto itself (ECDHE, RSA verification).
  Expect around a second, not milliseconds.
- The "Network sim" latency and loss settings still apply. They are **added on top** of the
  real network. Leave them at 0 to measure LTE alone.

## Limits

- One run at a time (MQTT *or* LwM2M). OTA stays a PC simulation.
- MQTT payloads up to about 3.5 KB (`MQTT_BUF_SIZE` 4096 on the board, 20 KB on the PC).
- The per-message CSV file isn't written (the board has no file system). The run summary
  still goes to `results/runs.jsonl`, tagged `"device":"stm32-ec200u"`.
- TLS: two CAs are built into the firmware: `ca.crt` (the local lab) and `mosquitto.org.crt`
  (test.mosquitto.org). Certificate **dates are not checked**, because the board has no
  trusted clock. To add a CA, list it in `ROMFS_FILES` in `CMakeLists.txt`.
- An idle UDP mapping through the cellular NAT can expire after about 30–60 s.
  If Leshan can't reach the device (a Read/Observe times out), keep notification intervals
  short or press the device-side **Notify** button. Any packet from the board opens the path
  again. This is the problem LwM2M queue mode solves on real deployments.
