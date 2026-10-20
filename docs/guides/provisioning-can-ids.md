# Provisioning CAN IDs and the board profile

Use [ESP-PROG-2-PY-CLI](https://github.com/XiaoKChen/ESP-PROG-2-PY-CLI) on the **board's own USB port** for first flash. It selects the partition table and writes the v2 CAN-ID/name block plus the [board profile](../reference/board-profile.md) in `cancfg`. An unprovisioned v2 block means IDs `0x79E/0x79F` and an empty INFO name; an absent/invalid profile means TWAI TX 4/RX 5 at 250 kbit/s with no holds. The records are validated independently. The IDU preset is both brain and node; its app answers APP_INFO/ENTER_UPDATER on `0x07/0x08`.

## Recommended: flash a SOLO preset

From the ESP-PROG-2-PY-CLI checkout, use its `esp-prog2-flasher` entry point. Supply the board's port; it is not auto-detected. With no board argument on a terminal, `--flash-solo` prompts for `odu` or `idu`.

```sh
uv run esp-prog2-flasher --flash-solo odu --port COM7 --bundle path/to/unzipped-bundle
uv run esp-prog2-flasher --flash-solo idu --port COM8 --bundle path/to/unzipped-bundle --app idu.bin
uv run esp-prog2-flasher --flash-solo --port COM8 --bundle path/to/unzipped-bundle
```

The bundle is `esp32s3-can-updater-<version>.zip`, containing one bootloader, one updater, both partition tables, blank `otadata` and `manifest.json`. From the first published release (`c-cpp-esp32-v1.2.0`, **not yet tagged** on this branch), use `--bundle-tag c-cpp-esp32-v1.2.0` instead of `--bundle` to fetch it with authenticated `gh`. Until then, use a locally built, unzipped [bundle](../getting-started/build.md#outputs). Do not pass both options. `--manufacturer NAME` overrides the preset manufacturer; preset product and IDs cannot be overridden. `--migrate-stock` first erases `0x10000..0x1FFFF` when moving from a stock Arduino layout. `--app app.bin` additionally writes an app to `ota_0` and selects it in `otadata`; without it the factory updater boots.

| Preset | Layout | TWAI TX/RX | Hold LOW | v2 product | CAN command/reply |
|---|---|---|---|---|---|
| `odu` | `node-4MB` | 4/5, 250 kbit/s | none | `SOLO ODU Controller` | `0x05/0x06` |
| `idu` | `idu-16MB` | 6/7, 250 kbit/s | GPIO3 | `SOLO IDU Controller` | `0x07/0x08` |

The tool optionally erases the stock region, then writes bootloader `0x0`, selected table `0x8000`, blank `otadata` `0xE000`, updater at the layout's `factory`, and 4 KB `cancfg` at `0x11000` in one `write-flash` with `dio`, `80m` and the layout's flash size. It reads back and compares `cancfg`. An optional app is written at the layout's `ota_0` with an OTA selection entry. It finishes with `--after watchdog-reset`; USB RTS hard reset did not reboot these boards on the bench. See [first flash](../getting-started/first-flash.md).

To re-provision an existing install without reflashing the updater, use
`--provision-node --port COM7 --product <name>`. For `SOLO ODU Controller` and
`SOLO IDU Controller` it writes and verifies the full 4 KB `cancfg` image (v2
block + that preset's board profile). For any other product it writes the
80-byte v2 block only, with the pair from `--boot-cmd-id`/`--boot-reply-id`
(`--manufacturer` is optional). esptool erases the whole 4 KB sector, so a
v2-only write leaves no profile: the updater then uses the defaults (GPIO4/5,
no holds). `--read-node-config --port COM7` shows the v2 settings, not the
profile.

## Manual esptool recipe (v2 + profile)

For a custom board, prepare a 4 KB `cancfg.bin`. This example uses the SOLO IDU preset; edit *both* records for other wiring, and validate the [profile rules](../reference/board-profile.md#record-v1). Run in a trusted local directory with Python 3.12 (`uv run --python 3.12 -`):

```sh
uv run --python 3.12 - <<'PY'
import struct
import zlib
from pathlib import Path

manufacturer, product = b"Normal Corporation", b"SOLO IDU Controller"
cmd, reply, tx, rx, bitrate, holds = 0x07, 0x08, 6, 7, 1, 1 << 3
assert len(manufacturer) <= 32 and len(product) <= 32
assert 0 <= cmd <= 0x7FF and 0 <= reply <= 0x7FF
v2 = struct.pack("<IBBBB", 0xDF0C0FDF, 2, len(manufacturer), len(product), 0xFF)
v2 += manufacturer.ljust(32, b"\xff") + product.ljust(32, b"\xff")
v2 += struct.pack("<HH", cmd, reply)
v2 += struct.pack("<I", zlib.crc32(v2))
profile = struct.pack("<IBBHBBBBQ", 0xDFB0A7D1, 1, 24, 0xFFFF,
                      tx, rx, bitrate, 0xFF, holds)
profile += struct.pack("<I", zlib.crc32(profile))
assert len(v2) == 80 and len(profile) == 24
assert profile.hex() == "d1a7b0df0118ffff060701ff0800000000000000cba2ca5d"
Path("cancfg.bin").write_bytes((v2 + profile).ljust(4096, b"\xff"))
PY
```

Use the *unzipped* universal bundle. For the IDU, `factory` is `0xFA0000` and `ota_0` is `0x20000`. For the ODU, use `partition-table-node-4MB.bin`, `--flash-size 4MB`, factory `0x20000` and adjust the records to the ODU values. These commands do not write an app, so blank `otadata` boots the updater. Replace `<port>` and `<bundle>`:

```sh
uv run --with esptool -m esptool --chip esp32s3 -p <port> --after watchdog-reset write-flash --flash-mode dio --flash-freq 80m --flash-size 16MB 0x0 <bundle>/bootloader.bin 0x8000 <bundle>/partition-table-idu-16MB.bin 0xe000 <bundle>/ota_data_initial.bin 0x11000 cancfg.bin 0xfa0000 <bundle>/can_updater.bin
uv run --with esptool -m esptool --chip esp32s3 -p <port> read-flash 0x11000 0x1000 cancfg-readback.bin
```

Compare `cancfg-readback.bin` byte-for-byte with `cancfg.bin` before use. If migrating from stock, erase `0x10000` for `0x10000` bytes *before* writing. Keep the bootloader/table/updater on the board's own layout; never use the normal app upload to overwrite `factory`. A v2-only re-provision without a profile makes the updater use ODU wiring and no holds.

## Using provisioned IDs

- Give each board on a shared bus a distinct command/reply pair. Replies need not be command + 1. Product names are not serial numbers.
- Discover with `GET /node/scan` or `can_flash.py --scan` ([INFO and discovery](../reference/can-protocol.md#info-and-discovery)). The default band is `0x700..0x71F`; widen it with `?from=&to=` or `--scan-from`/`--scan-to`.
- Supply **both** IDs for `/node/flash`, `can_flash.py --id-tx/--id-rx` or a package `--node` entry. Package `name=` compares the updater's product name exactly, including case and length.

Multi-board workflow: [can-flash guide](../../tools/can-flash/README.md#multi-board-workflow).
