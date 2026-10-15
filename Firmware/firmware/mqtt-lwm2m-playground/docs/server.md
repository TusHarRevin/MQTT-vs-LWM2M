# Servers: Mosquitto and Leshan

Both run on your PC. Pick Docker **or** the native scripts.

| | Ports |
|---|---|
| Mosquitto | 1883/tcp MQTT, 8883/tcp MQTT over TLS |
| Leshan | 8080/tcp web UI + REST, 5683/udp CoAP, 5684/udp CoAP over DTLS |
| lab (playground) | 8000/tcp dashboard, 7000/udp device events (localhost only) |

---

## Mosquitto

### Installation

- **Docker:** nothing to install; `docker compose up mosquitto` uses the
  official `eclipse-mosquitto:2` image.
- **Windows, portable:** `powershell -ExecutionPolicy Bypass -File scripts\get-servers.ps1`
  unpacks Mosquitto into `servers\.local\mosquitto` (needs 7-Zip). Otherwise
  `winget install EclipseFoundation.Mosquitto`.
- **Ubuntu/Debian/WSL:** `sudo apt-get install mosquitto mosquitto-clients`,
  then `sudo systemctl disable --now mosquitto` so the system broker doesn't
  hold port 1883. `scripts/get-servers.sh` does both.
- **macOS:** `brew install mosquitto`.

### Configuration

`servers/mosquitto/mosquitto.conf` (native) and `mosquitto.docker.conf`
(Docker) are identical except for certificate paths:

```conf
# lab default; see "Creating users"
allow_anonymous true
# $SYS statistics every 2 s (the dashboard reads these)
sys_interval 2
# plain MQTT
listener 1883
# MQTT over TLS
listener 8883
cafile certs/ca.crt
certfile certs/server.crt
keyfile certs/server.key
```

### Starting the broker

```bash
docker compose up mosquitto                            # Docker
scripts/start-mosquitto.sh                             # Linux/macOS/WSL
powershell -File scripts\start-mosquitto.ps1           # Windows
powershell -File scripts\start-mosquitto.ps1 -Port 21883 -TlsPort 28883   # other ports
```

The scripts run `mosquitto -c <config> -v` from `servers/mosquitto` so the
relative certificate paths resolve.

### Connecting a client

```bash
./build/mqtt_device --interval 1s                       # the simulated device
mosquitto_sub -h 127.0.0.1 -t 'playground/#' -v         # watch its telemetry
mosquitto_pub -h 127.0.0.1 -t 'playground/mqtt-dev-1/cmd' -m 'interval=5000'  # command it
mosquitto_sub -h 127.0.0.1 -t '$SYS/broker/clients/connected' -v              # broker stats
```

Topics used: `playground/<id>/telemetry` (device → broker) and
`playground/<id>/cmd` (broker → device; commands `interval=<ms>`, `qos=<0..2>`,
`publish`, `pause`, `resume`, `stop`).

### Creating users

```bash
cd servers/mosquitto
mosquitto_passwd -c passwd alice          # prompts for a password
```

Add to `mosquitto.conf` **before** the first `listener` line:

```conf
allow_anonymous false
password_file passwd
```

Restart the broker, then connect with `./build/mqtt_device --user alice --pass <password>`.
(The dashboard's monitor connects anonymously, so keep `allow_anonymous true` while using
the dashboard, or add a user for it.)

### Enabling TLS

TLS is already enabled on 8883 with a lab CA. To make new certificates:

```bash
sh servers/mosquitto/gen-certs.sh      # needs openssl (Git Bash has it)
```

Connect with verification of the broker certificate:

```bash
./build/mqtt_device --secure --ca servers/mosquitto/certs/ca.crt --tls-name localhost
mosquitto_sub -p 8883 --cafile servers/mosquitto/certs/ca.crt -h localhost -t 'playground/#' -v
```

`--tls12` caps the device at TLS 1.2 so you can compare it with TLS 1.3.
Details: [security.md](security.md).

### Viewing logs

The scripts run Mosquitto with `-v` in the foreground: every CONNECT,
SUBSCRIBE and PUBLISH is printed. With Docker: `docker compose logs -f mosquitto`.

---

## Leshan

### Installation

Leshan's demo server is a single jar (Java 17+):

- **Docker:** `docker compose up leshan` builds `servers/leshan/Dockerfile`
  (Temurin 21 + the jar from Maven Central).
- **Windows:** `scripts\get-servers.ps1` downloads the jar and a portable Java
  into `servers\.local`.
- **Linux/macOS:** `scripts/get-servers.sh` downloads the jar. Install Java
  with `sudo apt-get install openjdk-21-jre-headless` or `brew install openjdk`.

The version is pinned to **2.0.0-M18**.

### Starting the server

```bash
docker compose up leshan
scripts/start-leshan.sh                         # = java -Xmx256m -jar servers/.local/leshan-demo-server.jar
powershell -File scripts\start-leshan.ps1
```

Open http://127.0.0.1:8080.

### Registering a client

```bash
./build/lwm2m_device                  # endpoint "lwm2m-dev-1", plain CoAP to 127.0.0.1:5683
./build/lwm2m_device --id kitchen-1   # another endpoint name
```

The client appears in the Leshan UI's client list. The device prints
`REGISTER` and `REGISTER RESPONSE ACK 2.01`.

### Viewing resources

Click the endpoint in the Leshan UI. You'll see the objects the device
registered: Server (1), Device (3), BinaryAppDataContainer (19), Temperature
(3303), Humidity (3304) and Barometer (3315). Security (0) is never shown to a
server.

### Read, Write, Observe, Execute

In the UI, use the buttons next to each resource. For example:

| Operation | Try it on | What happens on the device |
|---|---|---|
| Read | `/3/0` or `/3303/0/5700` | `READ` → `READ RESPONSE 2.05` |
| Write | `/3303/0/5750` Application Type | `WRITE` → `2.04`, device logs the new value |
| Observe | `/3303/0/5700` | `OBSERVE` → `2.05`, then `NOTIFY` every interval |
| Execute | `/3303/0/5605` Reset min/max, `/3/0/4` Reboot (simulated), `/1/0/8` Registration update | `EXECUTE` → `2.04` |

### REST/API access

The Leshan demo server's REST API is what the UI uses. These calls were
checked against 2.0.0-M18 (`ep` = endpoint name):

```bash
curl http://127.0.0.1:8080/api/clients                                         # registered clients
curl "http://127.0.0.1:8080/api/clients/lwm2m-dev-1/3/0/9?timeout=5"           # READ battery
curl -X POST "http://127.0.0.1:8080/api/clients/lwm2m-dev-1/3303/0/5700/observe?timeout=5"   # OBSERVE
curl -X DELETE "http://127.0.0.1:8080/api/clients/lwm2m-dev-1/3303/0/5700/observe?active=true"  # cancel
curl -X PUT -H 'Content-Type: application/json' \
     -d '{"id":5750,"kind":"singleResource","value":"kitchen","type":"string"}' \
     "http://127.0.0.1:8080/api/clients/lwm2m-dev-1/3303/0/5750?timeout=5"     # WRITE
curl -X POST "http://127.0.0.1:8080/api/clients/lwm2m-dev-1/3303/0/5605?timeout=5"           # EXECUTE
curl -N http://127.0.0.1:8080/api/event                                        # live events (SSE)
```

DTLS pre-shared keys are managed with `/api/security/clients/`:

```bash
curl -X PUT -H 'Content-Type: application/json' \
  -d '{"endpoint":"lwm2m-dev-1","tls":{"mode":"psk","details":{"identity":"lwm2m-dev-1","key":"000102030405060708090a0b0c0d0e0f"}}}' \
  http://127.0.0.1:8080/api/security/clients/
curl -X DELETE http://127.0.0.1:8080/api/security/clients/lwm2m-dev-1
```

The dashboard reaches the same API through the lab at `/leshan/api/...`.

### Firmware update

LwM2M firmware update uses **Object 5**: write a package URI to `/5/0/1`, the
device downloads it, then execute `/5/0/2` (Update). This playground's device
**does not implement Object 5**. The OTA exercise is a separate
[simulation](ota.md) that teaches the same steps (download, store, verify,
activate) without pretending to be an LwM2M firmware update. Adding Object 5
is a natural next step once the basics are clear.
