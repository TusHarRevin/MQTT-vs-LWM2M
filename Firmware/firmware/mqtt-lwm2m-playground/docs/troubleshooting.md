# Troubleshooting

## Port 1883 is "forbidden" on Windows

Mosquitto prints *"An attempt was made to access a socket in a way forbidden
by its access permissions"*. Hyper-V, WSL2 or Docker Desktop reserve TCP port
ranges, and 1883 often falls inside one. Check:

```powershell
netsh int ipv4 show excludedportrange protocol=tcp
```

Fix it (in an **admin** PowerShell) by reserving 1883 for yourself before
WinNAT grabs it:

```powershell
net stop winnat
netsh int ipv4 add excludedportrange protocol=tcp startport=1883 numberofports=1
net start winnat
```

Or just use other ports:

```powershell
powershell -File scripts\start-mosquitto.ps1 -Port 21883 -TlsPort 28883
build\lab --mqtt-port 21883 --mqtts-port 28883
build\mqtt_device --port 21883
```

## "cannot connect to 127.0.0.1:1883 (tcp) - is the server running?"

Start the broker. Check it's listening with `netstat -an | findstr 1883`
(Windows) or `ss -ltn | grep 1883` (Linux).

## LwM2M device keeps sending REGISTER with no answer

- Leshan isn't running, or something else holds UDP 5683.
- Over DTLS (`--secure`): Leshan has no PSK for this endpoint, so the
  handshake fails (`DTLS handshake: SSL - The operation timed out`). Add it;
  see [security.md](security.md).

## REGISTER RESPONSE 4.03 Forbidden

Leshan has a PSK stored for this endpoint and refuses plain CoAP. Start with
Security ON, or delete it:
`curl -X DELETE http://127.0.0.1:8080/api/security/clients/lwm2m-dev-1`.

## LwM2M device registered but no notifications

Nothing is observing it. The dashboard observes automatically (checkbox
*Auto-OBSERVE*). From Leshan's UI, press *Observe* on a resource. A
`--count` run waits until notifications flow.

## Dashboard says Mosquitto/Leshan OFFLINE

The lab couldn't reach them on the configured ports. Run `build/lab --help`
and pass `--mqtt-port`, `--leshan-port`, etc.

## "control port 7101 busy"

Another `mqtt_device` is running; only one gets dashboard commands. Stop it,
or pass `--ctl-port 0`.

## "cannot listen on 127.0.0.1:8000"

Another lab (or another program) is using port 8000: `build/lab --http-port 8001`.

## First CMake configure fails while fetching

The first configure clones mbedTLS, Paho and Wakaama from GitHub. Behind a
proxy, set `HTTPS_PROXY`. Offline, point CMake at local copies:
`-DFETCHCONTENT_SOURCE_DIR_MBEDTLS=... -DFETCHCONTENT_SOURCE_DIR_PAHO=... -DFETCHCONTENT_SOURCE_DIR_WAKAAMA=...`

## Docker on Windows/macOS: LwM2M over UDP

Docker Desktop NATs UDP. Registration and observation work, but Leshan sees
the client at the Docker gateway address. If notifications stop after some
minutes of silence, the NAT mapping expired: lower `--lifetime` (e.g. 60) so
UPDATEs keep it alive.

## Leshan uses a lot of memory

The scripts start it with `-Xmx256m`. Close other heavy apps if Windows
reports low memory.
