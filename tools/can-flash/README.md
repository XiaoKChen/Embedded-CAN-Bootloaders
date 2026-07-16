# can-flash — CAN flashing tool for the UNO R4 Minima CAN bootloader

Streams a `.bin` application image to the custom CAN bootloader on the
Arduino UNO R4 Minima (Renesas RA4M1) over classic CAN 2.0B, using the
CAN Update Protocol v1.

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

## Wiring — the board has NO CAN transceiver

The Minima exposes CAN0 as logic-level pins only. You must wire an
external **5 V** CAN transceiver (e.g. **TJA1050**, **MCP2551**, or
**MCP2562**) between the board and the bus:

| Board pin | Transceiver |
|-----------|-------------|
| D4 (CTX)  | TXD |
| D5 (CRX)  | RXD |
| 5V        | VCC |
| GND       | GND (common ground with the adapter) |

Terminate the bus with **120 Ω** at both ends. Do not use a 3.3 V-only
transceiver (e.g. SN65HVD230) directly on this 5 V board.

## Entering CAN update mode

The bootloader accepts an update when either:

- the **application requests it**: it writes the magic `0xB007CAFE` to
  `R_SYSTEM->VBTBKR[4]` (unlock with `PRCR = 0xA502`, re-lock with
  `0xA500`) and resets, or
- **no valid application** is present in the slot — the board then stays
  in the bootloader waiting for an upload.

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
| HELLO | `0x01` | byte1 = protocol version (`0x01`) | ACK: byte3 = bootloader version, bytes4..7 = app slot size (uint32 LE, expect `0x0003B800` = 243,712) |
| ERASE | `0x02` | — | erases 120 blocks; ACK can take seconds (tool waits 15 s) |
| DATA | `0x03` | byte1 = seq (uint8, mod 256 over the whole transfer), bytes2..7 = 6 image bytes | no per-frame reply |
| SIZE | `0x04` | bytes2..5 = image size (uint32 LE) | |
| VERIFY | `0x05` | bytes2..5 = CRC32 of image (uint32 LE, IEEE/zlib) | |
| GO | `0x06` | — | boot the application |

Replies: ACK `{0x79, cmd_echo, 0x00, arg, ...}` / NAK `{0x79, cmd_echo, err, arg, ...}`
with err `0x01` unexpected seq (arg = seq to rewind to), `0x02` flash failure,
`0x03` CRC mismatch, `0x04` size out of bounds, `0x05` bad state.

**Windows:** one window = one 2048-byte flash block = 342 DATA frames
(341×6 + 1×2 bytes; the final image block may be short). Individual DATA
frames get no reply; after a full window the host must wait for the window
ACK/NAK — the bootloader stalls during flash writes and its RX FIFO is only
4 deep. On NAK `0x01` the host rewinds to the window containing the
requested seq and resends it from the start.
