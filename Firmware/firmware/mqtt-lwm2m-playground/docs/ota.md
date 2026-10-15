# SIMULATED OTA

> This is a **learning simulation**. It downloads, stores and verifies a
> file. It does **not** model STM32 flash (erase/program timing, banks), a
> bootloader, image signing, or rollback. Nothing here represents real
> device flash behaviour.

## Flow

```
Server  -- GET /ota/manifest?image=X -->  {"version","size","sha256"}     "firmware available"
Device  -- GET /ota/X  Range: bytes=a-b -->  one chunk per request        download
Device  -- write chunk -->  results/ota_storage/slot_b.bin                storage ("slot B")
Device  -- SHA-256(slot B) == manifest.sha256 ?                           verify
Device  -- write version -->  results/ota_storage/active_version.txt      activate
```

The lab server plays the firmware server. The download uses the same
link/netsim layer as the protocols, so the dashboard's network profile
slows it down and adds loss exactly like it does for MQTT. Chunked range
requests (default 1 KiB) mimic how an MCU behind a modem fetches an image,
one block at a time. A failed chunk is retried up to 3 times.

## Images

| Image | Size | Version |
|---|---|---|
| `firmware_v1.bin` | 64 KiB | 1.0.0 (generated at build time into `build/ota/`) |
| `firmware_v2.bin` | 256 KiB | 2.0.0 |
| `fw_<N>k.bin` | N KiB | 3.0.0, generated on demand by the lab (for size sweeps) |

Images are deterministic pseudo-random bytes with a small `PGFW` header
(`ota/fw_image.h`).

## Use

Dashboard: **OTA** tab → choose image, chunk size and network → **START
OTA**. **INTERRUPT OTA** aborts the download (the running version stays the
same). **RESET** wipes slot B and returns to v1.0.0. **Sweep firmware sizes**
plots *firmware size → OTA time*.

Command line (the lab must be running, since it serves the images):

```bash
./build/ota_device --image firmware_v2.bin --chunk 1024
./build/ota_device --image fw_512k.bin --chunk 4096 --profile poor
./build/ota_device --action reset
```

Measured and reported: firmware size, download time, verify time, total
time, wire bytes sent and received, number of chunks and retries, and
completion or the failure reason.
