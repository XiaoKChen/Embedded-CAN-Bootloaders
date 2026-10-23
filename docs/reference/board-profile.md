# ESP32-S3 board profile

The `cancfg` data partition (subtype `0x40`, offset `0x11000`, 4 KB) holds two independent records. Its first 80 bytes are the frozen v2 DFU configuration (`src/boot/dfu_config.h`): CAN command/reply IDs and manufacturer/product strings. At offset `0x50` comes the ESP32-only board profile (`src/boot/board_profile.h`): a 24-byte **v1** record or a 56-byte **v2** record. Fill the remainder with `0xFF`. Older v2-block readers stop at byte `0x50` and are unaffected. [Provisioning](../guides/provisioning-can-ids.md) writes both records.

The profile says what differs per board: TWAI pins and bitrate, GPIOs to hold LOW (v1, v2) or HIGH (v2) from the 2nd-stage bootloader onward, and (v2) up to eight I2C register writes that force an output chip safe, such as the SOLO ODU's EMC2305 fan controller.

## Record v2

All multi-byte integers are little-endian. Offsets are relative to `cancfg + 0x50`. Bytes `0x00..0x13` are laid out as in v1.

| Offset | Type | Field | Rule |
|---|---|---|---|
| `0x00` | u32 | magic | `0xDFB0A7D1` |
| `0x04` | u8 | version | `2` |
| `0x05` | u8 | length | `56` |
| `0x06` | u16 | reserved | encode as `0xFFFF`; ignored on read |
| `0x08` | u8 | TWAI TX GPIO | `0..48`, not refused |
| `0x09` | u8 | TWAI RX GPIO | `0..48`, not refused, different from TX |
| `0x0A` | u8 | bitrate code | `0` = 125 kbit/s, `1` = 250 kbit/s, `2` = 500 kbit/s, `3` = 1 Mbit/s |
| `0x0B` | u8 | reserved | encode as `0xFF`; ignored on read |
| `0x0C` | u64 | hold-LOW mask | bit n drives GPIOn LOW; no bits above 48, refused pins or TWAI pins |
| `0x14` | u64 | hold-HIGH mask | bit n drives GPIOn HIGH; hold-LOW rules, and no bit in common with the hold-LOW mask |
| `0x1C` | u8 | I2C SDA GPIO | `0xFF` = no I2C safe writes |
| `0x1D` | u8 | I2C SCL GPIO | |
| `0x1E` | u8 | I2C 7-bit address | `0x08..0x77` |
| `0x1F` | u8 | I2C write count | `0..8` |
| `0x20` | 8 x {u8, u8} | I2C writes | {register, value} pairs, written in order; encode unused pairs as `0xFF 0xFF`; unused pairs are ignored on read |
| `0x30` | u32 | reserved | encode as `0xFFFFFFFF`; ignored on read |
| `0x34` | u32 | CRC32 | IEEE/zlib CRC32 over record bytes `0x00..0x33` |

I2C rules: when SDA is `0xFF`, SCL and the address must also be `0xFF` and the count `0`. Otherwise SDA and SCL are `0..48`, distinct, not refused, not a TWAI pin and in neither hold mask; the address is `0x08..0x77`; the count is `1..8`.

## Record v1

| Offset | Type | Field | Rule |
|---|---|---|---|
| `0x00` | u32 | magic | `0xDFB0A7D1` |
| `0x04` | u8 | version | `1` |
| `0x05` | u8 | length | `24` |
| `0x06..0x13` | | as v2 | TWAI TX/RX, bitrate, reserved bytes, hold-LOW mask |
| `0x14` | u32 | CRC32 | IEEE/zlib CRC32 over record bytes `0x00..0x13` |

A v1 record reads as a v2 record with no hold-HIGH pins and no I2C writes.

## Validation and defaults

GPIO **0** (BOOT strap), **19/20** (USB D-/D+) and **26..32** (flash/PSRAM) are refused for TWAI, holds and I2C. Readers fetch 56 bytes and pick the layout from the version byte. Validate the entire record: a missing, short or invalid record (magic, version, length, CRC, pin, bitrate, mask or I2C field) selects *all* defaults: TX **4**, RX **5**, **250 kbit/s**, **no holds**, **no I2C writes**. Reserved bytes, unused write pairs and trailing bytes are ignored on read. A valid profile is independent of a valid v2 block.

### v1 and v2 compatibility

| Reader | v1 record | v2 record |
|---|---|---|
| Bootloader, updater and node-app helper up to `c-cpp-esp32-v1.2.1` | applied | **refused** (version check): defaults, so no holds and no writes |
| Bootloader and updater from `c-cpp-esp32-v1.3.0` | applied | applied |
| Arduino node-app helper from `c-cpp-esp32-v1.3.0` | hold-LOW pins | hold-LOW pins only |

Write a v2 record only together with a bundle that reads it (`c-cpp-esp32-v1.3.0` or later). A v2 record on an older bootloader silently drops every hold, including a v1-compatible hold-LOW mask. A board whose profile needs no hold-HIGH pins or I2C writes can keep a v1 record.

## What applies the profile

**2nd-stage bootloader** (every reset): right after `bootloader_init()`, before partition-table logging, it latches each held pin's level and then enables its output, so a pin never pulses to the wrong level. GPIOs may still float during ROM boot. It then runs the I2C safe writes and leaves both I2C pins released inputs (internal pull-ups on) for the app, logging `I2C safe writes 0x2C: 5/5 ACKed`.

**Factory updater**: it reads the record again and drives the holds and runs the I2C safe writes before health, USB or CAN service, then **repeats the writes every 1 s** for as long as it runs, parked or mid-update, logging `I2C safe writes 0x2C on SDA 8 SCL 9: 5/5 ACKed; repeating every 1000 ms` once. It starts TWAI on the recorded pins and bitrate, logging `record` or `defaults`. If TWAI init fails, it retries once on the default TWAI pins and bitrate; the holds and writes stay. If either default pin is held LOW, it **skips** that retry: holds remain asserted and the updater continues without CAN, with USB available.

**Arduino node-app helper** ([guide](../guides/platformio-arduino.md#stock-arduino-node-app-helper)): reads v1 or v2 and drives the hold-LOW pins. It does not drive hold-HIGH pins or run the I2C writes: the app owns those pins and its I2C bus. The app still owns and configures its TWAI controller.

### I2C safe writes

`src/boot/i2c_bitbang.c` bit-bangs the bus through the GPIO registers (bootloader) or the GPIO driver (updater), open-drain, at about 100 kHz:

1. Bus clear (UM10204 §3.1.16, as Embedded-PlatformIO `interface-i2c-bus`): release both lines; while SDA is LOW, clock SCL up to 9 times; then a STOP. A slave left mid-byte by a reset lets go of SDA.
2. One transaction per pair: START, address + W, register, value, STOP.

It is best effort: a NACKed write is skipped, and a bus that stays busy or an SCL held LOW past a 2 ms stretch budget ends the sequence. Nothing fails the boot or stops the updater. Bit timing for a full run with eight writes and the whole stretch budget is at most **4,475 us** (about 1.6 ms for the ODU's five writes on an idle bus), plus pin-access overhead.

Why the ODU needs it: the EMC2305 (DS20006532A §4.11) keeps its last fan duty across an ESP32 reset, and its power-up watchdog drives every fan to **full scale** if no Fan Setting register (`0x30`, `0x40`, `0x50`, `0x60`, `0x70`) is written within 4 s of EMC2305 power-up. Writing any Fan Setting register disables that watchdog. The bootloader write therefore stops the fans on every reset, and the updater's 1 s repeat catches an EMC2305 re-powered during an update session before its 4 s deadline.

## SOLO presets and golden bytes

The v2 block uses manufacturer `Normal Corporation`. The bytes below are the exact profile records, including CRC, pinned by `tests/test_board_profile.c` (v1) and `tests/vectors/board_profile_v2.txt` (v2), and by the provisioning tool's golden vectors. The separate v2-block CRC occupies `cancfg + 0x4C..0x4F`.

| Board | Layout | TWAI TX/RX | v2 product | CAN cmd/reply | v2-block CRC (LE) |
|---|---|---|---|---|---|
| SOLO ODU | `node-4MB` | 4/5, 250 kbit/s | `SOLO ODU Controller` | `0x05/0x06` | `02bd587b` |
| SOLO IDU | `idu-16MB` | 6/7, 250 kbit/s | `SOLO IDU Controller` | `0x07/0x08` | `e12a8269` |

**SOLO ODU v2**: hold LOW GPIO 1, 2, 6, 17, 18, 33, 34, 35, 47, 48 (mask `0x0001800E00060046`); hold HIGH GPIO 21 (mask `0x0000000000200000`); EMC2305 at `0x2C` on SDA GPIO8 / SCL GPIO9, five writes `{0x30,0x00} {0x40,0x00} {0x50,0x00} {0x60,0x00} {0x70,0x00}` (Fan 1..5 Setting = 0 %):

```text
d1a7b0df0238ffff040501ff460006000e800100000020000000000008092c0530004000500060007000ffffffffffffffffffff89eb5aaf
```

**SOLO IDU v2**: hold LOW GPIO3 (`1 << 3`); no hold HIGH, no I2C writes:

```text
d1a7b0df0238ffff060701ff08000000000000000000000000000000ffffff00ffffffffffffffffffffffffffffffffffffffff46c50b86
```

**v1 records** (for bootloaders up to `c-cpp-esp32-v1.2.1`):

| Board | Hold LOW | Profile record (hex) |
|---|---|---|
| SOLO ODU | none | `d1a7b0df0118ffff040501ff0000000000000000209e783a` |
| SOLO IDU | GPIO3 (`1 << 3`) | `d1a7b0df0118ffff060701ff0800000000000000cba2ca5d` |

The IDU is both a brain and a CAN node: its application must answer APP_INFO and ENTER_UPDATER on its own `0x07/0x08` pair. Unprovisioned v2 IDs instead default to `0x79E/0x79F`; those defaults are independent of the profile's GPIO defaults.
