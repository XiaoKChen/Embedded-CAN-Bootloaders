# Provisioning per-device CAN IDs and the INFO name

Optional. Unprovisioned boards use `0x79E` (host→board) / `0x79F`
(board→host) and answer INFO with an empty name. To give a board its own IDs
(several parked boards on one bus) and a name for `--scan`, write the 80-byte
v2 config block (`src/boot/dfu_config.h`, same format as the RA4M1) to the
`cancfg` partition (`0x11000`, [flash-layout.md](../concepts/flash-layout.md)).
The updater and `examples/node_app` both read it, so a node answers on the
same pair whether parked or running.

A dedicated provisioning tool is tracked in issue #7. Until then, build the
block by hand:

```python
import struct, zlib

MANUFACTURER = b"Normal"            # <= 32 ASCII bytes
PRODUCT      = b"IDU-Controller-A"  # <= 32 ASCII bytes, reported by INFO / --scan
CMD_ID, REPLY_ID = 0x700, 0x711     # 11-bit host->board / board->host IDs

block  = struct.pack("<IBBBB", 0xDF0C0FDF, 2, len(MANUFACTURER), len(PRODUCT), 0xFF)
block += MANUFACTURER.ljust(32, b"\xff") + PRODUCT.ljust(32, b"\xff")
block += struct.pack("<HH", CMD_ID, REPLY_ID)
block += struct.pack("<I", zlib.crc32(block))
assert len(block) == 80
open("cancfg.bin", "wb").write(block)
```

```sh
python -m esptool --chip esp32s3 write_flash 0x11000 cancfg.bin
```

A missing, unreadable or invalid block (bad magic/CRC/version) falls back to
the defaults; an out-of-range ID falls back individually. The updater never
faults on it. A first flash with `erase-flash` leaves the block
unprovisioned; a USB reflash without it preserves `cancfg`.

## Using provisioned IDs

- **Unique pairs.** Multiple parked nodes need distinct command IDs and
  unambiguous reply IDs; the reply need not be command + 1. Names are not
  device serial numbers: provision unique command/reply IDs on a shared bus.
- **Discover** with `GET /node/scan` on the brain or `can_flash.py --scan`
  ([can-protocol.md, INFO and discovery](../reference/can-protocol.md#info-and-discovery)).
  The default band is `0x700..0x71F`; widen it with `?from=&to=` or
  `--scan-from`/`--scan-to`.
- **Address** a node with **both** IDs: `?cmd=0x701&reply=0x702` on the brain,
  `--id-tx 0x700 --id-rx 0x711` for can-flash, `cmd=…,reply=…` in a package
  `--node` entry. The package `name=` check compares the INFO name exactly
  (case and length).
- A running `examples/node_app` logs `(per-device, cancfg)` and answers only
  on its own pair.

Multi-board workflow with can-flash:
[tools/can-flash/README.md](../../tools/can-flash/README.md#multi-board-workflow).
