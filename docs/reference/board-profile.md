# ESP32-S3 board profile

The `cancfg` data partition (subtype `0x40`, offset `0x11000`, 4 KB) holds two independent records. Its first 80 bytes are the frozen v2 DFU configuration (`src/boot/dfu_config.h`): CAN command/reply IDs and manufacturer/product strings. At offset `0x50` comes the ESP32-only 24-byte board profile (`src/boot/board_profile.h`). Fill the remainder with `0xFF`. Older v2 readers stop at byte `0x50` and are unaffected. [Provisioning](../guides/provisioning-can-ids.md) writes both records.

## Record v1

All multi-byte integers are little-endian. Offsets in this table are relative to `cancfg + 0x50`.

| Offset | Type | Field | Rule |
|---|---|---|---|
| `0x00` | u32 | magic | `0xDFB0A7D1` |
| `0x04` | u8 | version | `1` |
| `0x05` | u8 | length | `24` |
| `0x06` | u16 | reserved | encode as `0xFFFF`; ignored on read |
| `0x08` | u8 | TWAI TX GPIO | `0..48`, not refused |
| `0x09` | u8 | TWAI RX GPIO | `0..48`, not refused, different from TX |
| `0x0A` | u8 | bitrate code | `0` = 125 kbit/s, `1` = 250 kbit/s, `2` = 500 kbit/s, `3` = 1 Mbit/s |
| `0x0B` | u8 | reserved | encode as `0xFF`; ignored on read |
| `0x0C` | u64 | hold-LOW mask | bit n drives GPIOn LOW; no bits above 48, refused pins or TWAI pins |
| `0x14` | u32 | CRC32 | IEEE/zlib CRC32 over record bytes `0x00..0x13` |

GPIO **0** (BOOT strap), **19/20** (USB D-/D+) and **26..32** (flash/PSRAM) are refused for both TWAI and holds. Validate the entire record: a missing, short or invalid record (magic, version, length, CRC, pin, bitrate or mask) selects *all* defaults: TX **4**, RX **5**, **250 kbit/s**, **no holds**. Reserved bytes are ignored on read; trailing bytes are ignored. A valid profile is independent of a valid v2 block.

The 2nd-stage bootloader reads the record and drives validated holds LOW immediately after `bootloader_init()`, before partition-table logging. GPIOs may still float during ROM boot. The factory updater reads it again and drives holds before USB, health or CAN service. It starts TWAI on the recorded pins and bitrate, logging `record` or `defaults`. If TWAI init fails, it retries once on the defaults. If either default pin is held LOW, it **skips** that retry: holds remain asserted and the updater continues without CAN, with USB available. The optional [Arduino node-app helper](../guides/platformio-arduino.md#stock-arduino-node-app-helper) reads the v2 IDs and profile holds. The app still owns and configures its TWAI controller.

## SOLO presets and golden bytes

The v2 block uses manufacturer `Normal Corporation`. The bytes below are the exact 24-byte profile records, including CRC, pinned by `tests/test_board_profile.c` and the provisioning tool's golden vectors. The separate v2 CRC occupies `cancfg + 0x4C..0x4F`.

| Board | Layout | TWAI TX/RX | Hold LOW | v2 product | CAN cmd/reply | v2 CRC (LE) | Profile record (hex) |
|---|---|---|---|---|---|---|---|
| SOLO ODU | `node-4MB` | 4/5, 250 kbit/s | none | `SOLO ODU Controller` | `0x05/0x06` | `02bd587b` | `d1a7b0df0118ffff040501ff0000000000000000209e783a` |
| SOLO IDU | `idu-16MB` | 6/7, 250 kbit/s | GPIO3 (`1 << 3`) | `SOLO IDU Controller` | `0x07/0x08` | `e12a8269` | `d1a7b0df0118ffff060701ff0800000000000000cba2ca5d` |

The IDU is both a brain and a CAN node: its application must answer APP_INFO and ENTER_UPDATER on its own `0x07/0x08` pair. Unprovisioned v2 IDs instead default to `0x79E/0x79F`; those defaults are independent of the profile's GPIO defaults.
