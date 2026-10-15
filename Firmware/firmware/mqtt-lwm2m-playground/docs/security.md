# Security playground

Start without security so the protocols are easy to read in the inspector.
Then switch it on and see what it costs.

| | Off | On |
|---|---|---|
| MQTT | MQTT over TCP, port 1883 | MQTT over **TLS** (1.3 by default, `--tls12` for 1.2), port 8883 |
| LwM2M | CoAP over UDP, port 5683 | CoAP over **DTLS 1.2 with a pre-shared key**, port 5684 |

Both use **mbedTLS 3.6** inside `device/common/link.c`. The protocol code
doesn't know whether it's encrypted.

## What gets measured

- **Handshake time:** from the first ClientHello to handshake completion.
- **Handshake bytes:** wire bytes in each direction during the handshake.
- **Connection time ("time to ready"):** socket + handshake + protocol
  session. For MQTT that's CONNECT/CONNACK; for LwM2M it's REGISTER/2.01.
- **Bytes per message:** wire bytes, including TLS/DTLS record overhead.

The device prints them, the dashboard shows them on the device panel, and the
*Security OFF vs ON* experiment charts them side by side.

## MQTT over TLS

The broker uses a certificate signed by a lab CA (`servers/mosquitto/certs`).
The device verifies the broker against that CA and the name `localhost`:

```bash
./build/mqtt_device --secure --ca servers/mosquitto/certs/ca.crt --tls-name localhost
./build/mqtt_device --secure --ca servers/mosquitto/certs/ca.crt --tls-name localhost --tls12
```

Without `--ca`, the connection is encrypted but the broker isn't
authenticated. The device warns *SERVER NOT VERIFIED* in that case. Don't do
that outside a lab.

The lab CA and its private key are committed so the first TLS run works out
of the box. **They are for this lab only.** Regenerate them with
`servers/mosquitto/gen-certs.sh`.

## LwM2M over DTLS-PSK

Both sides must know the same identity and key. The server has to learn
them first:

```bash
curl -X PUT -H 'Content-Type: application/json' \
  -d '{"endpoint":"lwm2m-dev-1","tls":{"mode":"psk","details":{"identity":"lwm2m-dev-1","key":"000102030405060708090a0b0c0d0e0f"}}}' \
  http://127.0.0.1:8080/api/security/clients/
./build/lwm2m_device --secure
```

(The dashboard does the PUT for you when Security is ON.) Defaults:
identity = endpoint name, key = `000102…0f` (`--psk-id`, `--psk`). Cipher
suites offered: `TLS_PSK_WITH_AES_128_CCM_8` (the LwM2M-mandated one) and
`TLS_PSK_WITH_AES_128_CBC_SHA256`.

**Two things that trip people up:**

1. If Leshan has **no** PSK for the endpoint, its DTLS handshake fails. The
   device times out and Wakaama retries later.
2. If Leshan **has** a PSK for the endpoint, it rejects a *plain-CoAP*
   REGISTER with **4.03 Forbidden**. The device explains this in its log. The
   dashboard deletes the PSK when you start with Security OFF; by hand, use
   `curl -X DELETE http://127.0.0.1:8080/api/security/clients/lwm2m-dev-1`.

## What this does not cover (yet)

Client certificates (MQTT mTLS), raw public keys / X.509 for LwM2M,
bootstrap servers, OSCORE, and secure key storage on the MCU.
