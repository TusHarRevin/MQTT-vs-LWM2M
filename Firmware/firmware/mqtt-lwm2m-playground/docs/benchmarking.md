# Benchmarking

## What is recorded

Each run writes one record per message to `results/<run>.csv`:

```
seq,timestamp_ms,latency_ms,payload_bytes,bytes_sent,bytes_received,success,retries
```

- `bytes_sent` / `bytes_received`: **wire** bytes for that message's exchange,
  counted at the socket. That includes TLS/DTLS overhead, the QoS 0 probe,
  PUBREL and CoAP retransmissions.
- A failed message has an **empty latency**. Nothing is estimated.

At the end of a run the device prints a summary and the lab appends it to
`results/runs.jsonl`. The comparison tab and charts read that file.

## Latency definitions

| Protocol | Measured from → to |
|---|---|
| MQTT QoS 0 | PUBLISH → PINGRESP of a PINGREQ sent right after it (the broker answers in TCP order, so this proves receipt; see [mqtt.md](mqtt.md)) |
| MQTT QoS 1 | PUBLISH → PUBACK |
| MQTT QoS 2 | PUBLISH → PUBCOMP |
| LwM2M | CON → ACK of each notification, from its first transmission |

Statistics are over **successful** messages only: minimum, average, median,
P95, P99, maximum. Percentiles use the **nearest-rank** method (the
⌈p/100·n⌉-th smallest value). With 100 messages, P99 is the 99th value. With
20 messages, P95 and P99 are both the maximum, so don't read much into
percentiles from small runs.

"Connection" is time to ready: socket + TLS/DTLS handshake + CONNECT/CONNACK
(MQTT) or REGISTER/2.01 (LwM2M).

## The first benchmark

Dashboard → **MQTT vs LwM2M** → **Benchmark: 100 + 100**. It runs 100 MQTT
messages, then 100 LwM2M notifications, one every 0.2 s, using the QoS,
security and payload chosen on the protocol tabs. Or from a shell:

```bash
./build/mqtt_device  --qos 1 --interval 200ms --count 100
./build/lwm2m_device --interval 200ms --count 100     # then OBSERVE /3303/0/5700 in Leshan
```

Experiments in the dashboard (each one runs the real devices against the real servers):

| Button | Runs |
|---|---|
| Sweep packet loss | 0, 1, 2, 5, 10 % × both protocols, 50 messages each |
| Sweep network latency | 0, 50, 100, 250, 500, 1000 ms × both, 20 messages each |
| Sweep payload size | natural, 256, 512, 1024 B × both (+ 4096 B MQTT) |
| Security OFF vs ON | plain vs TLS/DTLS × both, 30 messages each |
| OTA: Sweep firmware sizes | 32 KiB … 1 MiB images |

## Network simulation

Two options.

### 1. Built-in (default; works on Windows, macOS and Linux)

`device/common/netsim.c` sits between the protocols and the socket, under
TLS/DTLS, so encrypted records are affected too.

- **Latency:** a delay line in each direction. Every outgoing packet is held
  for the one-way latency before it reaches the socket, and every incoming
  packet is held for it before the device sees it. A request/response
  therefore gains 2 × latency, a QoS 2 exchange 4 ×, a DTLS 1.2 PSK handshake
  (3 round trips) 6 ×, however many packets are in flight.
- **Loss, UDP (LwM2M):** datagrams really are dropped in either direction.
  CoAP and DTLS must retransmit.
- **Loss, TCP (MQTT):** dropping bytes would corrupt the stream, so a "lost"
  segment costs one retransmission timeout instead (`--rto`, 200 ms by
  default, doubling for consecutive losses). That's what the kernel's TCP
  would do.

Profiles: Excellent 0 ms/0 %, Good 50 ms/1 %, Poor 250 ms/5 %,
Very Poor 1000 ms/10 %, or Custom.

This is a teaching model: no jitter, no reordering, no bandwidth limit, no
congestion control. Delayed packets wait in the device process, so they move
whenever the device sends or waits for data, which a polling client does
constantly.

### 2. `tc netem` (Linux or WSL2, root)

Packet-accurate, done by the kernel, and it affects the real TCP stack too.

```bash
sudo scripts/netem.sh on 100 5     # 100 ms each way, 5 % loss on ports 1883 8883 5683 5684
sudo scripts/netem.sh show
sudo scripts/netem.sh off
```

Run the servers, lab and devices inside the same Linux/WSL environment, and
set the dashboard's network to 0 ms / 0 % so you don't impair twice.

## Reading results honestly

- Loopback numbers are tiny. They show protocol overhead, not radio
  behaviour. Use the network profiles to see the differences that matter on
  NB-IoT/LTE-M.
- MQTT under "loss" never fails on TCP; it gets slower. LwM2M can fail: a
  notification is given up after 4 retransmissions with doubling timeouts,
  62–93 s after it was first sent (with the default 2 s ACK_TIMEOUT).
- Compare like with like: same network, same payload, enough messages
  (≥ 100 for P95, more for P99).
