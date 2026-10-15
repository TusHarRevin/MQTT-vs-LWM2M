# LwM2M in the playground

`lwm2m_device` is an LwM2M 1.1 client built on **Eclipse Wakaama** (pinned
to the 2026-05-26 snapshot). Wakaama implements the protocol state machine
and CoAP. `device/lwm2m/lwm2m_client.c` provides Wakaama's transport
callbacks (`lwm2m_connect_server`, `lwm2m_buffer_send`, …) on top of the
playground's UDP/DTLS link, and traces every CoAP message.

## Objects

| Path | Object / resource | Ops | Notes |
|---|---|---|---|
| `/0/0` | Security | – | server URI, security mode (PSK or NoSec), keys; never readable by the server |
| `/1/0/1` | Server · Lifetime | RW | default 300 s (`--lifetime`) |
| `/1/0/7` | Server · Binding | RW | `U` (UDP) |
| `/1/0/8` | Server · Registration Update Trigger | E | sends an UPDATE |
| `/3/0/0..3` | Device · Manufacturer, Model, Serial, Firmware version | R | |
| `/3/0/4` | Device · Reboot | E | simulated: resets the sensor model |
| `/3/0/9` | Device · **Battery Level** (%) | R | |
| `/3/0/11` | Device · Error Code | R | multi-instance, `0` = no error |
| `/3/0/13..15` | Device · Current Time, UTC Offset, Timezone | RW | |
| `/3/0/20` | Device · Battery Status | R | 0 normal, 4 low (< 20 %) |
| `/19/0/0` | BinaryAppDataContainer · Data | R | the telemetry JSON, sized by `--payload` |
| `/3303/0/5700` | Temperature · Sensor Value (Cel) | R | also 5601/5602 min/max, 5603/5604 range, 5701 units |
| `/3303/0/5605` | Temperature · Reset Min and Max | E | |
| `/3303/0/5750` | Temperature · Application Type | RW | try a Write here |
| `/3304/0/5700` | Humidity (%RH) | R | same resources as 3303 |
| `/3315/0/5700` | Barometer (hPa) | R | same resources as 3303 |

> **Battery:** OMA's core objects have no stand-alone "Battery" object;
> battery level is resource 9 of the Device object, `/3/0/9`. `/3/0/13` is
> *Current Time*.

All objects share one small table-driven engine (`objects.c`): a resource
table plus get/set/execute callbacks. That's a compact pattern that also suits
an MCU.

## Transactions

```
REGISTER        DEVICE -- CON POST /rd?ep=..&lt=300&lwm2m=1.1&b=U  (link-format payload) --> SERVER
                DEVICE <-- ACK 2.01 Created, Location /rd/<id> ------------------------- SERVER
READ            SERVER -- CON GET /3/0/9 -->  DEVICE -- ACK 2.05 + value -->
WRITE           SERVER -- CON PUT /3303/0/5750 + value -->  DEVICE -- ACK 2.04 -->
EXECUTE         SERVER -- CON POST /3303/0/5605 -->  DEVICE -- ACK 2.04 -->
OBSERVE         SERVER -- CON GET /3303/0/5700 Observe:0 -->  DEVICE -- ACK 2.05 Observe:n + value -->
NOTIFY          DEVICE -- CON 2.05 Observe:n+1 + value (same token) -->  SERVER -- empty ACK -->
UPDATE          DEVICE -- CON POST /rd/<id> -->  SERVER -- ACK 2.04 -->
DEREGISTER      DEVICE -- CON DELETE /rd/<id> -->  SERVER -- ACK 2.02 Deleted -->
```

In the event log, each line shows the CoAP type, code, URI and Message ID.
The inspector shows the raw bytes.

## Confirmable notifications

Wakaama sends Observe notifications as **NON** (non-confirmable). Nothing
acknowledges them, so their delivery and latency can't be measured, and a
lost one is simply gone.

The playground marks each outgoing notification **CON** (RFC 7641 allows
either) and takes over its reliability, using RFC 7252 timing:

- wait `ACK_TIMEOUT` × random(1 … 1.5): 2–3 s by default (`--ack-timeout`)
- retransmit the same message (same Message ID), doubling the timeout
- give up after `MAX_RETRANSMIT` = 4 retransmissions

Latency is **CON → ACK**, measured from the *first* transmission. Under
packet loss you'll see `NOTIFY (retransmission)` lines, and the latency
distribution grows a long tail (2 s, 6 s, 14 s …). That's CoAP's backoff at
work.

A notification can also end in **RST** from the server. That counts as failed:

- Leshan rejects a *retransmitted* notification when it already has a newer
  one for that observation (the old value is stale).
- When a second OBSERVE replaces an observation, notifications still in
  flight carry the old token and are rejected. Only one tool (the dashboard
  *or* the Leshan UI) should observe a resource during a benchmark.

`--notify-non` (or *NON* in the dashboard) restores Wakaama's default:
latency then shows "not measured", which is the honest answer.

## Try it

```bash
./build/lwm2m_device --interval 1s
# in Leshan (http://127.0.0.1:8080): open lwm2m-dev-1, Observe /3303/0/5700
./build/lwm2m_device --interval 500ms --count 50 --loss 10   # watch retransmissions
```

Dashboard device buttons: **Register** / **Deregister** send the real
REGISTER / DE-REGISTER messages; **Notify** makes the device sample now.

## Server-side view

The lab follows Leshan's event stream (`/api/event`). The LwM2M panel's
"Notifications received" is the count of NOTIFICATION events **Leshan**
emitted, not the device's own count.
