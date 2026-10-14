# can-flash — CAN flashing tool for the ESP32-S3 CAN updater

Streams a `.bin` application image to the ESP32-S3 CAN (TWAI) updater (the
`factory` app built from this branch) over classic CAN 2.0B, using the CAN
Update Protocol v1 — the same protocol, byte for byte, as the UNO R4 Minima
(RA4M1) CAN bootloader on the `version/c-cpp/ra4m1` branch. Flash the app's
`build/<project>.bin` (the image ESP-IDF writes to an OTA slot), not a merged
or bootloader image.

## Requirements

- Python 3.9+
- `python-can >= 4.0` (`pip install -r requirements.txt`)
- `can-isotp >= 2.0` — for the `--enter-bootloader` pre-step
- `textual >= 0.60` — for the interactive TUI
- A CAN adapter supported by python-can (SocketCAN, slcan/CANable,
  gs_usb/candleLight, PCAN, Kvaser, ...)

The tool declares its dependencies inline (PEP 723), so the simplest way to
run it — pulling `python-can`, `can-isotp`, and `textual` automatically — is:

```sh
uv run can_flash.py app.bin --interface slcan --channel COM5
```

## TUI vs headless CLI

The tool runs two ways from the same file:

- **Interactive TUI** — run it with **no arguments** (or pass `--tui`):

  ```sh
  uv run can_flash.py
  ```

  A Textual UI opens with inputs for the CAN **interface/channel/bitrate**
  (defaults `socketcan` / `can0` / `250000`), a firmware **`.bin` path** with a
  **Browse** file picker (directory tree filtered to `.bin`), board **RX/TX**
  IDs (defaults `0x07`/`0x08`) plus an **Enter bootloader first** toggle, a
  **Skip GO** toggle, an **Identify** button (queries a parked bootloader via
  CMD_INFO `0x07` and shows its product name, or "no reply"), and a **Flash**
  button that runs the full HELLO→SIZE→ERASE→DATA→VERIFY→GO flow on a background
  thread with a live progress bar and log. The engine is exactly the same
  `CanFlasher` / `flash_image` code path the CLI uses.

- **Headless CLI** — pass a firmware path and/or any flag and it runs exactly as
  documented below, printing to the terminal and exiting with a status code.

  ```sh
  uv run can_flash.py app.bin --interface slcan --channel COM5
  ```

## Wiring — an external 3.3 V transceiver is required

The ESP32-S3's TWAI controller drives logic-level pins only. Wire a **3.3 V**
CAN transceiver (e.g. **SN65HVD230**, **TCAN332**, **MCP2562** with VIO at
3.3 V) between the board and the bus:

| ESP32-S3 | Transceiver |
|----------|-------------|
| TWAI TX GPIO (menuconfig, default GPIO4) | TXD |
| TWAI RX GPIO (menuconfig, default GPIO5) | RXD |
| 3V3      | VCC (VIO) |
| GND      | GND (common ground with the adapter) |

Terminate the bus with **120 Ω** at both ends. Do not feed a 5 V-logic
transceiver's RXD straight into an ESP32-S3 pin.

## Entering CAN update mode

The updater accepts an update whenever it is running, which is when:

- the **application requests it**: it calls
  `can_updater_client_enter_updater()` (component
  `components/can_updater_client`), which selects the `factory` partition and
  restarts — see `docs/build.md`, or
- **no valid application** is selected (fresh board, or a rolled-back app) —
  the board then stays in the updater waiting for an upload.

Once entered, the board stays in the updater across resets until an image is
flashed (unlike the RA4M1, where a reset returns to the app).

### Rebooting a running app remotely (`--enter-bootloader`)

If the board is running the **A7-IDU-Controller** app you don't have to
reset or park it by hand. Pass `--enter-bootloader` and the tool first
calls the app's Airform `/enter_bootloader` RPC over CAN; that handler
writes the `0xB007CAFE` handoff word and resets the MCU into the CAN
bootloader. The tool then waits ~2 s for the reboot and runs the normal
flash.

```sh
# reboot the running app into the bootloader, then flash
uv run can_flash.py app.bin --enter-bootloader --interface slcan --channel COM5

# just reboot into the bootloader and exit (no firmware)
uv run can_flash.py --enter-bootloader --interface slcan --channel COM5
```

Details:

- The RPC request is a `Uart_Packet` (`Uart_Communication` library) for the
  route path `/enter_bootloader`, sent as **standard ISO-TP 15765-2** (First
  Frame → the board's Flow Control → Consecutive Frames) because it exceeds a
  single 8-byte CAN frame.
- The board is addressed by its Airform board IDs: the host sends the request
  to CAN ID `--board-rx` (default `0x07`) and receives Flow Control on
  `--board-tx` (default `0x08`). These are the IDU Controller's `{RX, TX}` and
  are independent of the bootloader's own `--id-tx`/`--id-rx`.
- **No RPC response is expected** — the handler resets the MCU and never
  replies, so the tool only confirms the ISO-TP transfer completed (it got the
  Flow Control), then waits for the reboot. If no Flow Control arrives the tool
  errors out (wrong board IDs, or the app isn't running the route).
- This requires the target to be running the app **with the
  `/enter_bootloader` route** (A7-IDU-Controller) **and** the custom
  bootloader flashed. Without the route there is nothing to answer the RPC;
  without the custom bootloader the handoff magic is ignored on reset.

## Discovering multiple parked boards (`--scan`)

Boards no longer have to share the legacy bootloader IDs `0x79E`/`0x79F`. Each
board can be provisioned with its **own** `boot_cmd_id` (host→board) and
`boot_reply_id` (board→host); a board with no provisioned pair falls back to the
`0x79E`/`0x79F` default. This lets several boards sit parked in the bootloader on
one bus at the same time without colliding.

`--scan` finds every one of them. For each candidate cmd ID it sends the INFO
probe (CMD `0x07`) and listens **broadly** — on *any* reply arbitration ID, not
a single fixed one — because a board's reply ID is arbitrary. A valid answer is
the 5-frame INFO reply (header `{0x79, 0x07, name_len, ver, FF…}` + the 32-byte
product-name field); the tool records `(cmd_id → reply_id, name)`. Only one board
answers per cmd ID, so IDs are probed one at a time and never collide.

By default it probes the band `0x700..0x71F` **plus** the legacy `0x79E`
**plus** the known-device cmd IDs `0x01, 0x03, 0x05, 0x07, 0x09` (the
host→boot IDs the ESP-Prog-2 flasher provisions from the `airform-server-dev`
board table). Override the band with `--scan-from`/`--scan-to`; `0x79E` and the
known-device IDs are **always** included, so provisioned known boards are found
without touching `--scan-from`/`--scan-to`. The per-ID timeout is short, so a
full default scan takes only a few seconds.

```sh
# list every parked board on the bus
uv run can_flash.py --scan --interface slcan --channel COM5

# widen (or narrow) the probed cmd-ID band
uv run can_flash.py --scan --scan-from 0x700 --scan-to 0x73F --interface slcan --channel COM5
```

Output is a table of `NAME  CMD_ID → REPLY_ID  PROTO`. Then flash a specific
board by passing its pair to a normal flash run:

```sh
uv run can_flash.py app.bin --id-tx 0x700 --id-rx 0x711 --interface slcan --channel COM5
```

### Multi-board workflow

1. **Assign per-board IDs** with the ESP-Prog-2 flasher when provisioning each
   board, so every board has a unique `boot_cmd_id`/`boot_reply_id` pair.
2. **Park the boards** in the bootloader (no valid app, the app's handoff magic,
   or `--enter-bootloader`) — several can be parked on the bus simultaneously.
3. **`--scan`** the bus to list all parked boards with their cmd/reply IDs.
4. **Flash each** board in turn with its own `--id-tx <cmd_id> --id-rx <reply_id>`.

> Real multi-board discovery needs actual CAN hardware; the `--selftest` scan
> check exercises the same code path against fake bootloaders on a virtual bus.

## Usage

```
python can_flash.py firmware.bin --interface slcan --channel COM5 --bitrate 250000
```

`--interface`, `--channel`, and `--bitrate` are passed straight through to
`can.Bus`. Default bitrate is 250000.

Per adapter type:

```sh
# SocketCAN (Linux)
sudo ip link set can0 up type can bitrate 250000
python can_flash.py app.bin --interface socketcan --channel can0

# slcan (CANable, USBtin, Lawicel; serial CDC device)
python can_flash.py app.bin --interface slcan --channel COM5            # Windows
python can_flash.py app.bin --interface slcan --channel /dev/ttyACM0    # Linux/macOS

# gs_usb / candleLight (on Linux these usually appear as SocketCAN instead)
python can_flash.py app.bin --interface gs_usb --channel 0
```

Options:

| Option | Default | Meaning |
|--------|---------|---------|
| `--id-tx` | `0x79E` | host → bootloader CAN ID |
| `--id-rx` | `0x79F` | bootloader → host CAN ID |
| `--bitrate` | `250000` | CAN bitrate |
| `--retries` | `3` | retry limit per protocol step |
| `--no-go` | off | flash and verify, but do not start the app |
| `--enter-bootloader` | off | reboot a running app into the bootloader via the Airform `/enter_bootloader` RPC first; then flash (or, with no firmware, just reboot and exit) |
| `--board-rx` | `0x07` | target board Airform RX CAN ID (host → board) for the `/enter_bootloader` RPC |
| `--board-tx` | `0x08` | target board Airform TX CAN ID (board → host, Flow Control) |
| `--info` | off | ask the parked bootloader (at `--id-tx`/`--id-rx`) for its product name and exit |
| `--scan` | off | discover **all** parked boards, each on its own cmd/reply IDs, and print a table (see below); run it alone, then flash with `--id-tx`/`--id-rx` |
| `--scan-from` | `0x700` | first candidate cmd ID to probe with `--scan`, inclusive |
| `--scan-to` | `0x71F` | last candidate cmd ID to probe with `--scan`, inclusive |
| `--tui` | off | launch the interactive TUI (also the default when run with no arguments) |
| `--selftest` | off | run the full flow against an in-process fake bootloader (no hardware) |

`--scan-from`/`--scan-to` take any base (`0x700`, `1792`). The legacy `0x79E`
and the known-device cmd IDs (`0x01`–`0x09`) are always probed in addition to
the band, so a board on the default IDs, or a provisioned known board, is found
even outside the custom range.

Exit code is 0 on success, non-zero on any failure.

### Selftest

```
python can_flash.py --selftest
```

Runs HELLO→SIZE→ERASE→DATA→VERIFY→GO against a fake bootloader thread on
python-can's `virtual` interface, including a forced sequence-gap NAK to
exercise the window-rewind path, and asserts the received image matches.
It also brings up **two** fake bootloaders on distinct cmd/reply ID pairs and
asserts `--scan`'s broad listen discovers both (correct name, cmd, and reply ID)
while ignoring a decoy cmd ID no board answers. It also checks a golden vector:
that the encoded `/enter_bootloader`
`Uart_Packet` matches the exact bytes the device's `Uart_Communication`
library would parse, and a headless TUI smoke test (via Textual's
`App.run_test()`) that the TUI mounts with all its key widgets and that
argument routing picks the TUI for bare/`--tui` invocations. No hardware
required.

## Protocol summary (CAN Update Protocol v1)

Classic CAN 2.0B, 11-bit IDs, every frame DLC 8, unused bytes `0xFF`.
Host transmits on `0x79E`, bootloader replies on `0x79F`.
Required order: HELLO → SIZE → ERASE → DATA windows → VERIFY → GO.

| Cmd | byte0 | Payload | Notes |
|-----|-------|---------|-------|
| HELLO | `0x01` | byte1 = protocol version (`0x01`) | ACK: byte3 = bootloader version, bytes4..7 = app slot size (uint32 LE, ESP32-S3: `0x001C0000` = 1,835,008) |
| ERASE | `0x02` | — | ESP32-S3: opens the OTA slot and answers at once (sectors are erased as DATA reaches them); tool waits up to 15 s |
| DATA | `0x03` | byte1 = seq (uint8, mod 256 over the whole transfer), bytes2..7 = 6 image bytes | no per-frame reply |
| SIZE | `0x04` | bytes2..5 = image size (uint32 LE) | |
| VERIFY | `0x05` | bytes2..5 = CRC32 of image (uint32 LE, IEEE/zlib) | |
| GO | `0x06` | — | boot the application |

Replies: ACK `{0x79, cmd_echo, 0x00, arg, ...}` / NAK `{0x79, cmd_echo, err, arg, ...}`
with err `0x01` unexpected seq (arg = seq to rewind to), `0x02` flash failure,
`0x03` CRC mismatch, `0x04` size out of bounds, `0x05` bad state, `0x06` image
rejected by the target (ESP32-S3: VERIFY's CRC matched but `esp_ota_end` refused
the image — not an esp32s3 app, or a bad SHA-256).

**Windows:** one window = one 2048-byte flash block = 342 DATA frames
(341×6 + 1×2 bytes; the final image block may be short). Individual DATA
frames get no reply; after a full window the host must wait for the window
ACK/NAK — the bootloader stalls during flash writes and its RX FIFO is only
4 deep. On NAK `0x01` the host rewinds to the window containing the
requested seq and resends it from the start. Window start seqs repeat
every 128 windows, so the rewind is resolved against the current and the
next window only (an ESP32-S3 slot spans 896 windows).
