# can-flash — CAN flashing tool for the UNO R4 Minima CAN bootloader

Streams a `.bin` application image to the custom CAN bootloader on the
Arduino UNO R4 Minima (Renesas RA4M1) over classic CAN 2.0B, using the
CAN Update Protocol v1.

## Requirements

- Python 3.9+
- `python-can >= 4.0` (`pip install -r requirements.txt`)
- A CAN adapter supported by python-can (SocketCAN, slcan/CANable,
  gs_usb/candleLight, PCAN, Kvaser, ...)

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
| `--selftest` | off | run the full flow against an in-process fake bootloader (no hardware) |

Exit code is 0 on success, non-zero on any failure.

### Selftest

```
python can_flash.py --selftest
```

Runs HELLO→SIZE→ERASE→DATA→VERIFY→GO against a fake bootloader thread on
python-can's `virtual` interface, including a forced sequence-gap NAK to
exercise the window-rewind path, and asserts the received image matches.

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
