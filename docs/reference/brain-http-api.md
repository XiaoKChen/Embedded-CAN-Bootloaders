# Brain HTTP API

`brain_ota` serves HTTP on **port 80** of its W5500 Ethernet (DHCP; the IP is
in its boot log, see [first-flash.md](../getting-started/first-flash.md#the-brain-brain_ota)).
Source: `brain/main/http_ota_server.c`, `node_flash.c`, `package_update.c`.

**Bench-only, trusted LAN:** no authentication or TLS; packages are unsigned.
In PowerShell write `curl.exe`: plain `curl` is an alias for
`Invoke-WebRequest` there.

| Method | Path | Query | Runs on | Success |
|---|---|---|---|---|
| GET | [`/`](#get-) | — | HTTP task | 200 plain text |
| POST | [`/ota`](#post-ota) | — | worker | 200, then reboot |
| GET | [`/node/scan`](#get-nodescan) | `from`, `to` | worker | 200 JSON |
| POST | [`/node/flash`](#post-nodeflash) | `cmd`, `reply` | worker | 202, then CAN flash |
| POST | [`/node/recover`](#post-noderecover) | `cmd`, `reply` | worker | 202, then pulses + INFO |
| GET | [`/node/app_info`](#get-nodeapp_info) | `cmd`, `reply` | worker | 200 JSON |
| GET | [`/node/status`](#get-nodestatus) | — | HTTP task | 200 JSON |
| POST | [`/package`](#post-package) | — | worker | 202, then brain + relay |
| GET | [`/package/status`](#get-packagestatus) | — | HTTP task | 200 JSON |

## Common rules

- **One worker, 409 when busy.** All `/ota`, `/node/scan`, `/node/flash`,
  `/node/recover`, `/node/app_info` and `/package` jobs share one static
  worker. An overlapping request gets **409** with
  `An update/scan/recovery, startup self-test, or retained package job is active; poll status`,
  in both directions and during staging, CAN flashing, validation and relay.
  Startup reserves that worker until the Ethernet self-test finishes and the
  package pending-record check has run, so conflicting jobs receive 409 even
  during startup. A retained package record that cannot be safely cleared
  (or an unconfirmed brain holding one) also blocks mutating jobs with 409.
- **Status stays live.** `GET /`, `/node/status` and `/package/status` are
  answered by the HTTP task and remain responsive while any job runs (except
  the actual brain reboot).
- **Early rejects.** Rejections (409/411/413 and similar) are sent as soon as
  the headers are read, then the connection closes without draining the body.
  With a large body still being sent, Windows curl may report `000` /
  "Connection was reset" and drop the reply; `-v` shows the status line, or
  send a small body to see it cleanly.
- **Uploads** need a nonzero `Content-Length` (`curl.exe --data-binary` sends
  the file unmodified, with that header); chunked uploads are reported as
  length 0 → 411. A receive gap of **10 s** mid-body aborts the upload (408
  attempted; the reply may not get out). A peer close mid-body aborts without
  a reply. Every body is written to flash in 4 KB chunks, never held in RAM.
- **CAN IDs** (`cmd`, `reply`, `from`, `to`): decimal or `0x` hexadecimal
  only, `0..0x7FF`; no signs, whitespace or URL encoding. Omitted IDs default
  to `0x79E` / `0x79F` (scan: `0x700` / `0x71F`). JSON reports IDs in decimal
  (`1950` = `0x79E`, `1951` = `0x79F`).
- **JSON strings** from devices (names, versions) and package status text are
  JSON-escaped. Exception: see the `/node/recover` note below.
- Replies other than JSON are `text/plain`, one line.

## `GET /`

Plain-text status:

```sh
curl.exe http://<ip>/
```

```
project:   brain_ota
version:   v1.0.2-...
esp-idf:   v5.3.6
running:   ota_0 (0x20000)
ota state: VALID
next slot: ota_1
ip:        192.168.1.57
uptime:    42 s
```

`ota state` is one of `VALID`, `PENDING_VERIFY`, `NEW`, `INVALID`, `ABORTED`,
`UNDEFINED`, or `UNDEFINED (no otadata entry)` (first boot after a USB flash).

## `POST /ota`

The brain's own image: a raw `brain_ota.bin` as the body.

```sh
curl.exe --data-binary "@brain/build/brain_ota.bin" http://<ip>/ota
```

It streams into the inactive slot (`ota_0`/`ota_1`, 3 MB). Expect a few
seconds, not well under one: the 100 Mbit link is not the bottleneck. Erasing
and writing the flash sector by sector is, with the 20 MHz SPI to the W5500
after it. On success the reply is `OTA ok: N bytes; rebooting into the new image`
(HTTP 200) and the brain restarts about 1 s later (1000 ms) into the new
slot, which must pass the self-test ([system-overview.md](../concepts/system-overview.md#the-brain)).
Every error leaves the boot partition unchanged:

| Status | Cause |
|---|---|
| 411 | no `Content-Length` (e.g. a chunked upload) |
| 413 | image larger than the 3 MB slot |
| 409 | the running image is still `PENDING_VERIFY`, or another job is active (OTA/node upload/node flash/scan/recovery/app_info/package, startup, retained package record) |
| 400 | not an app image, or rejected by validation (`ESP_ERR_OTA_VALIDATE_FAILED`) |
| 408 | no data for 10 s mid-upload (the reply may not get out) |
| 500 | flash error |

Reply texts: `OTA rejected: <status> (<error>)` before any byte is written,
`OTA aborted at byte <n>: <error>` mid-body, `OTA rejected: <error>; boot partition unchanged`
at the end, `OTA aborted: receive timeout`.

**Only upload brain_ota images.** Any valid ESP32-S3 app is accepted and
booted. One that confirms itself but has no Ethernet (the bench app, for
instance) can then only be replaced over USB.

## `GET /node/scan`

Discovers parked node updaters with INFO.

```sh
curl.exe http://<ip>/node/scan
curl.exe "http://<ip>/node/scan?from=0x700&to=0x73F"
```

Default scan: sorted unique `0x700..0x71F`, plus `0x79E` and known command
IDs `1,3,5,7,9`. Overrides replace only the range; the extra IDs are always
probed. Each probe has a **300 ms total INFO deadline**, listens on any
standard reply ID, and requires the header plus four name frames on the
same ID. An empty/default scan takes about **11.4 s**. With one unprovisioned
node, expect:

```json
{"nodes":[{"cmd_id":1950,"reply_id":1951,"name":"","protocol":1}],"truncated":false}
```

| Status | Cause |
|---|---|
| 200 | JSON as above |
| 400 | bad range/IDs: `Expected 0 <= from <= to <= 0x7FF` |
| 409 | worker busy |
| 502 | CAN transport error (TX cannot drain); body is the host diagnostic |
| 503 | `CAN link unavailable` |

The brain stores at most 64 scan results; `truncated:true` asks you to narrow
the range. Names are JSON-escaped (including embedded control characters):
every name byte is sent as a `\uXXXX` escape, non-ASCII as U+FFFD. A running
app does not answer INFO, so it is not listed.

## `POST /node/flash`

Stages a node app `.bin` in `node_fw`, then flashes it over CAN.

```sh
curl.exe -i --data-binary "@examples/bench_app/build/bench_app.bin" "http://<ip>/node/flash?cmd=0x79E&reply=0x79F"
curl.exe http://<ip>/node/status
```

The same IDs are defaults if the query is omitted. For a provisioned node,
use **both IDs from scan**; the reply need not be command + 1. Send the app
`.bin`, never a bootloader or merged flash image.

The brain erases `node_fw` sector by sector, writes each chunk and reads it
back for byte equality. **202 Accepted is returned after the complete body is
verified in staging, without waiting for CAN flashing.** The worker then sends
ENTER_UPDATER (a running node app parks itself; on its ACK the brain waits
1.5 s for the updater; an updater NAKs at once) and runs
HELLO → SIZE → ERASE → DATA → VERIFY → GO from that partition. A complete
upload whose 202 response cannot be sent is not flashed.

| Status | Body / cause |
|---|---|
| 202 | `Node image staged; poll /node/status` |
| 400 | `cmd/reply must be standard CAN IDs (0..0x7FF)` |
| 408 | `Upload receive timeout` |
| 409 | worker busy |
| 411 | `Nonempty Content-Length required` |
| 413 | `Image exceeds node_fw partition` (the **2 MB staging partition**), before erase |
| 500 | `node_fw erase failed`, `node_fw write/read-back failed` |
| 503 | `CAN or writable unencrypted node_fw partition unavailable` |

Length and validity of the staged image are RAM-only: partial/stale `node_fw`
contents are never resumed after a brain reset. A staged image bigger than
the node's announced **1.75 MB slot** fails at SIZE, before node ERASE.

## `POST /node/recover`

Forces the node into its updater with GPIO3 pulses, then probes INFO.

```sh
curl.exe -i -X POST http://<ip>/node/recover
curl.exe -i -X POST "http://<ip>/node/recover?cmd=0x79E&reply=0x79F"
curl.exe http://<ip>/node/status
```

POST returns **202** (`Recovery started; poll /node/status`), 400 for
invalid IDs, 409 when busy, 503 `CAN link unavailable`. For provisioned
nodes supply both actual IDs. The worker sends four pulses (GPIO3 HIGH
100 ms, LOW 400 ms, finally LOW; [recovery.md](../concepts/recovery.md)),
waits 1 s, then probes INFO; no ENTER_UPDATER is sent. An absent node can
take over 8 s to fail INFO because the host retries internally (four 2 s
attempts). `success` means the brain reached the updater.

Note: the success text embeds the node's name in literal double quotes,
which `/node/status` does not escape, so that one response is not valid JSON
when it appears (see `node_flash_recover_run`).

## `GET /node/app_info`

Identifies a running node app with APP_INFO (three 0.5 s attempts).

```sh
curl.exe "http://<ip>/node/app_info?cmd=0x79E&reply=0x79F"
```

| Node state | Response |
|---|---|
| node app running | `200 {"cmd_id":1950,"reply_id":1951,"mode":"app","protocol":1,"project":"node_app","version":"p6-next"}` |
| parked in the updater | `200 {"cmd_id":1950,"reply_id":1951,"mode":"updater"}` |
| bench_app, or no node | `504` (`APP_INFO: timeout (attempts 3)`) after about 1.5 s |
| invalid reply | `502` with the host diagnostic |
| bad IDs / busy / no CAN | `400` / `409` / `503` |

Strings are JSON-escaped. The APP_INFO probe of a parked updater leaves its
session untouched; a following `/node/flash` must still succeed.

## `GET /node/status`

Status of the last standalone node job (`/node/flash` or `/node/recover`):

```json
{"state":"flashing","percent":42,"bytes":81920,"total":191264,"status":"CAN transfer; awaiting VERIFY and GO"}
```

States: `idle`, `staging`, `flashing`, `recovering`, `success`, `failed`.
Bytes/percent refer to staged bytes while staging and accepted image bytes
while flashing; `percent` = `bytes` × 100 / `total`. **100% is not success
until VERIFY and GO are ACKed.** After a brain reboot the status is `idle`.

| `status` text | When |
|---|---|
| `Receiving into node_fw` | staging |
| `Sending ENTER_UPDATER over CAN` | flashing, before HELLO |
| `CAN handshake`, `CAN handshake (entered updater via CAN)` | flashing, HELLO |
| `CAN transfer; awaiting VERIFY and GO` | flashing, DATA |
| `GO: success ...`, ending with `(entered updater via CAN)` when the app was parked over CAN | success |
| host diagnostic, e.g. `HELLO: timeout (attempts 4)`, `DATA window 0 not accepted after 4 attempts`, `SIZE rejected: size out of bounds 0x04 ...`, `VERIFY rejected: image rejected 0x06 ...` | failed |
| `Upload receive timeout`, `Upload connection closed`, `node_fw erase failed`, `node_fw write/read-back failed`, `Upload response connection closed; CAN not started` | staging failed; no CAN session started |
| `Recovery pulses; awaiting INFO` | recovering |
| `recovered: node answers INFO (name "<name>", protocol <n>)` | recovery success |
| `no INFO after recovery pulses (<diagnostic>)`, `recovery pulses failed: <error>` | recovery failed |

On a flash the brain logs its final `CAN health` (bus state, TX/RX error
counters, dropped frames). Package progress is not reported here; see
`/package/status`.

## `POST /package`

Uploads a v1 update package ([package-format.md](package-format.md)).

```sh
curl.exe -i --data-binary "@update.ecbpkg" http://<ip>/package
curl.exe http://<ip>/package/status
```

202 is sent only after staging/read-back, manifest validation, every payload
SHA, image-envelope check and slot-size check. No nodes are touched during
upload/validation. After 202 the worker updates/reboots the brain first if
needed, then relays NODE entries in table order.

| Status | Body / cause |
|---|---|
| 202 | `Package staged and checked; poll /package/status` (staged and checked, **not finished**) |
| 408 | `Package receive timeout` |
| 409 | `Running brain image is not confirmed`, or worker busy |
| 411 | `Nonempty Content-Length required` (missing/zero length) |
| 413 | `Package exceeds pkg partition`, before erase |
| 422 | manifest/hash/envelope/slot errors, e.g. `Payload SHA-256 mismatch or flash read failed` |
| 500 | `pkg erase failed`, `pkg write/read-back failed` |
| 503 | `Writable unencrypted pkg partition and NVS required; USB-reflash new table` (unavailable storage) |

Accepted asynchronous failures appear in status rather than changing the
already-sent 202 response. A timeout or peer close never starts a relay.

## `GET /package/status`

Returns JSON, with all text/name/result strings escaped:

```json
{"state":"relaying","status":"Relaying node entries in manifest order","bytes":686208,"total":686208,"brain":"skipped_same","nodes":[{"index":1,"cmd_id":1950,"reply_id":1951,"name":"","state":"flashing","percent":42,"result":"CAN transfer; awaiting VERIFY and GO"}]}
```

- Package state: `idle`, `receiving`, `validating`, `updating_brain`, `relaying`,
  `done`, `failed`. Sequence: `receiving → validating → updating_brain` (if
  needed), reboot, `validating → relaying → done`; without a changed brain,
  skip reboot. A fatal package/brain failure is `failed`. `done` can contain
  failed/skipped nodes. The terminal status is RAM-only: it remains until the
  next request/reset.
- `bytes`/`total`: received package bytes, retained through validation/relay.
- `brain`: `none`, `skipped_same`, `updated`, `rolled_back`; during
  `updating_brain`, `updated` describes the intended step, not completed self-test.
- Node `index`: zero-based **manifest** index, not node-array position.
- Node state: `pending`, `probing`, `recovering`, `flashing`, `ok`, `failed`, `skipped`.
  Percent is accepted transfer bytes; 100 alone is not success. Check `brain`,
  each node's `state` and `result`, not just `percent:100`.

Package `status` texts: `Receiving into pkg; active images unchanged`,
`Validating manifest and every payload from flash`,
`Writing and verifying inactive brain OTA slot`,
`Brain verified; rebooting before node relay`,
`Relaying node entries in manifest order`, `Package complete`,
`Relay finished with node failures; inspect results`,
**`brain update rolled back; node relay aborted`**,
`Brain not confirmed VALID; relay retained but blocked until next boot`
(state `updating_brain`), and failure texts such as
`Brain image invalid or OTA read-back SHA mismatch; node relay aborted`,
`NVS unavailable; jobs blocked to preserve any pending relay`,
`Unknown or damaged pending record; jobs blocked (service required)`.

Node `result` texts, in relay order: `Sending ENTER_UPDATER over CAN`,
`INFO probe`, `No CAN entry; pulsing GPIO3 (only the wired node can recover)`,
`CAN handshake (<path>)`, `CAN transfer; awaiting VERIFY and GO`, then
`ok; <path>`, `product name mismatch; node not flashed (<path>)`,
`<diagnostic> (<path>)`, or
`<diagnostic> (no updater even after ENTER_UPDATER and GPIO3 pulses)`. `<path>`
names how the node reached its updater: `updater already parked`,
`entered updater via CAN` or `recovered via GPIO3 pulses`.
Relay behaviour: [package-format.md](package-format.md#brain-first-transaction-and-reboot-record).
