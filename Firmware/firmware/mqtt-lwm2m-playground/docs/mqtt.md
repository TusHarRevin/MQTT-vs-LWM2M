# MQTT in the playground

`mqtt_device` is an MQTT 3.1.1 client written in C. Eclipse Paho's
**MQTTPacket** library encodes and decodes the packets
(`MQTTSerialize_*` / `MQTTDeserialize_*`). `device/mqtt/mqtt_client.c` owns
the socket, so it can time each exchange and count every byte. That's the
same split you'd use on a microcontroller.

## Session

```
DEVICE                                  BROKER
  | TCP connect (3-way handshake)          |
  | [TLS handshake]          --secure      |
  | CONNECT  client_id, keepalive, clean ->|
  |<- CONNACK  return_code 0               |
  | SUBSCRIBE playground/<id>/cmd  QoS 1 ->|
  |<- SUBACK  granted QoS                  |
  | PUBLISH playground/<id>/telemetry  --->|  every --interval
  |   ... ack depends on QoS ...           |
  | PINGREQ (only when idle 75 % of keepalive)
  |<- PINGRESP                             |
  | DISCONNECT ---------------------------->|
```

## Payload

```json
{"dev":"mqtt-dev-1","seq":12,"ts":1730000000000,"temperature":27.41,
 "humidity":62.10,"pressure":1012.30,"battery":87}
```

About 120 bytes. `--payload N` adds a `"pad"` field so the payload is exactly
N bytes, up to 20 KB.

## QoS

| QoS | Flow | Guarantee | Latency measured as |
|---|---|---|---|
| 0 | PUBLISH | at most once | PUBLISH → PINGRESP of a trailing PINGREQ (see below) |
| 1 | PUBLISH → PUBACK | at least once | PUBLISH → PUBACK |
| 2 | PUBLISH → PUBREC → PUBREL → PUBCOMP | exactly once | PUBLISH → PUBCOMP |

**Why a PINGREQ for QoS 0?** QoS 0 has no acknowledgement, so "broker
receipt" isn't directly observable. MQTT runs over one ordered TCP stream,
and the broker processes it in order. So when a PINGREQ sent right after the
PUBLISH gets its PINGRESP, the broker must already have read the PUBLISH.
The probe adds 2 + 2 bytes per message, which show up in "bytes per message".
`--no-probe` turns it off; QoS 0 latency is then reported as *not measured*.

## Try it

```bash
./build/mqtt_device --qos 0 --interval 500ms --count 20
./build/mqtt_device --qos 2 --interval 500ms --count 20
./build/mqtt_device --qos 1 --latency 100 --loss 5 --count 50    # bad network
```

The dashboard's **Send cmd via broker** button publishes `interval=2000` to
the device's command topic, so you see the broker deliver a PUBLISH *to* the
device (and the device's PUBACK).

## Server-side view

The lab keeps its own MQTT connection and subscribes to
`playground/+/telemetry` and to Mosquitto's `$SYS/broker/...` statistics. The
dashboard's broker panel therefore shows what the broker actually delivered
("Received (subscriber)"), next to what the device says it published.
