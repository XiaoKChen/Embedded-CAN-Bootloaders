# CAN Update Protocol v1

The node updater speaks the same **CAN Update Protocol v1** as the RA4M1
bootloader on `version/c-cpp/ra4m1`. The engine (`src/boot/can_update.c`,
frame layouts in `src/boot/can_update.h`) is shared, host-tested pure C
behind a slot HAL. Hosts: `tools/can-flash/can_flash.py` (Python) and
`components/can_flash_host` (C, used by the brain).

## Frames

Classic CAN 2.0B, 11-bit IDs, every frame DLC 8, unused bytes `0xFF`. Default
IDs `0x79E` (host→board) / `0x79F` (board→host), or a per-device pair from
the `cancfg` block (same 80-byte v2 format the RA4M1 parses,
`src/boot/dfu_config.h`; [provisioning](../guides/provisioning-can-ids.md)).
Required order: `HELLO → SIZE → ERASE → DATA windows → VERIFY → GO`, 2 KB
ACK-paced windows, CRC32 (IEEE/zlib) over the image, plus `INFO (0x07)` for
discovery (`--scan`).

| Cmd | byte0 | Payload | Notes |
|-----|-------|---------|-------|
| HELLO | `0x01` | byte1 = protocol version (`0x01`) | ACK: byte3 = bootloader version, bytes4..7 = app slot size (uint32 LE, ESP32-S3: `0x001C0000` = 1,835,008) |
| ERASE | `0x02` | — | ESP32-S3: opens the OTA slot and answers at once (sectors are erased as DATA reaches them); hosts wait up to 15 s |
| DATA | `0x03` | byte1 = seq (uint8, mod 256 over the whole transfer), bytes2..7 = 6 image bytes | no per-frame reply |
| SIZE | `0x04` | bytes2..5 = image size (uint32 LE) | |
| VERIFY | `0x05` | bytes2..5 = CRC32 of image (uint32 LE, IEEE/zlib) | |
| GO | `0x06` | — | boot the application |
| INFO | `0x07` | — | updater identifies itself (5-frame reply) |
| APP_INFO | `0x08` | byte1 = protocol version | answered by a **running app** only (9-frame reply) |
| ENTER_UPDATER | `0x09` | byte1 = protocol version, bytes2..5 = key `0xB007CAFE` (uint32 LE) | running app ACKs, then restarts into its updater |

Replies: ACK `{0x79, cmd_echo, 0x00, arg, ...}` / NAK `{0x79, cmd_echo, err, arg, ...}`
with err `0x01` unexpected seq (arg = seq to rewind to), `0x02` flash failure,
`0x03` CRC mismatch, `0x04` size out of bounds, `0x05` bad state, `0x06` image
rejected by the target.

**Windows:** one window = one 2048-byte flash block = 342 DATA frames
(341×6 + 1×2 bytes; the final image block may be short). Individual DATA
frames get no reply; after a full window the host must wait for the window
ACK/NAK. On NAK `0x01` the host rewinds to the window containing the
requested seq and resends it from the start.

## ESP32-S3 deltas

All handled by `tools/can-flash` on this branch (the wire format is unchanged):

- HELLO announces a **1.75 MB** slot (896 windows). Window start seqs repeat
  every 128 windows, so hosts resolve a rewind NAK against the current/next
  window only.
- New NAK code **`0x06`** on VERIFY: the CRC matched, but `esp_ota_end`
  rejected the image (not an esp32s3 app, bad SHA-256).
- ERASE answers immediately; flash sectors are erased as DATA reaches them.

## INFO and discovery

The INFO reply is 5 frames: header `{0x79, 0x07, name_len, ver, FF…}` + the
32-byte product-name field. INFO's five frames are unnumbered: an
incomplete/foreign burst is retried (or a scan miss), not guessed.
Provisioned names are ASCII, at most 32 bytes; unprovisioned boards answer
with an empty name.

A scan sends INFO to each candidate command ID and listens on **any**
standard reply ID, because a board's reply ID is arbitrary; it requires the
header plus four name frames on the same ID. Default candidates: the band
`0x700..0x71F`, plus `0x79E`, plus the known-device command IDs
`0x01, 0x03, 0x05, 0x07, 0x09`. Multiple parked nodes need distinct command
IDs and unambiguous reply IDs. Brain specifics:
[`/node/scan`](brain-http-api.md#get-nodescan); PC: `can_flash.py --scan`
([tool README](../../tools/can-flash/README.md#discovering-multiple-parked-boards---scan)).

## App control: APP_INFO and ENTER_UPDATER

A running app (`examples/node_app`) answers `APP_INFO (0x08)` and
`ENTER_UPDATER (0x09)` instead of INFO; the updater NAKs those as unknown
commands, so a host tells app from updater (`src/boot/app_control.h`).
A running app built on `examples/node_app` (`main/can_service.c`) answers
two native single-frame commands on the node's own update IDs (no ISO-TP):

- **APP_INFO (`0x08`)**: a 9-frame reply with the app's project name and
  version (ESP app descriptor);
- **ENTER_UPDATER (`0x09`, key `0xB007CAFE`)**: ACK, then a clean restart into
  the updater (`can_updater_client_enter_updater()`; not a recovery crash).

It never answers HELLO/INFO/... (it is not an updater). App-control replies
keep the ACK/NAK layout with arg = `0xA5` (the app marker). The APP_INFO ACK
is 9 frames: header `{0x79, 0x08, 0x00, 0xA5, proto, version_len, project_len, check}`,
then the 32-byte version field and the 32-byte project-name field (4 frames
each, `0x00`-padded); `check` = low byte of CRC32 over header bytes 4..6 plus
both fields, so a partial or foreign burst is rejected
(`src/boot/app_control.h`).

### Telling app, updater and silence apart

The updater's engine NAKs both opcodes as unknown commands,
`{0x79, cmd, 0x05, 0x00, ...}` (`err 0x05, arg 0x00`), in every phase without
touching its session. So:

| Reply to APP_INFO / ENTER_UPDATER | Meaning |
|---|---|
| ACK/NAK with arg `0xA5` | a running app with the CAN service |
| NAK err `0x05`, arg `0x00` | the updater (already parked) |
| silence | neither: no node on those IDs, or an app without the CAN service (e.g. `examples/bench_app`) |

Hosts send ENTER_UPDATER (up to three 0.5 s attempts) and, on an app ACK, wait
1.5 s for the updater to boot before INFO/HELLO.

## Host implementations and known limits

`components/can_flash_host/include/can_flash_host.h` documents the portable
C API and typed diagnostics. The host tests run against the **real**
`src/boot/can_update.c`, not a permissive Python fake. Four attempts, 2 s
ordinary ACK, 15 s ERASE, 150 ms quiet drain, 2048-byte windows and current/
next-window rewind resolution match the Python defaults. The ESP-IDF adapter
rounds the 1 ms early receive poll up to one RTOS tick so it actually yields.
A continuously busy receive drain is bounded at 2 s rather than hanging.

Intentional differences / protocol limits:

- The Python `_command` calls commands idempotent; the real engine accepts
  SIZE, ERASE and VERIFY only in their exact phase. Losing those ACKs can
  cause a retry to get STATE. Restart the full upload rather than treating
  that NAK as success. A lost GO ACK is ambiguous: the app may already run.
- The Python host retries a lost final DATA ACK, but the real node is already
  TRANSFERRED and rejects DATA. The C host first probes VERIFY on a final
  DATA timeout: ACK proves delivery and CRC; STATE safely permits DATA retry;
  CRC/image rejection fails closed. It does not send VERIFY again after ACK.
- SIZE is sent even if the announced slot is smaller, obtaining the node's
  authoritative SIZE NAK rather than a Python-side size exception. No ERASE
  follows a rejected SIZE.
- INFO's unnumbered frames: see above.

Host verification: [host-tests.md](../testing/host-tests.md).
