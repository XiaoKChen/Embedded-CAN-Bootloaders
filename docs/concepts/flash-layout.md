# Flash layout

The universal bundle uses **one bootloader and one factory updater**, with a different partition table per flash size. `esptool write-flash --flash-size 4MB|16MB` patches the common bootloader image header at write time. The updater does not use PSRAM. The first 80 bytes of `cancfg` are the v2 identity/ID block; the 24-byte [board profile](../reference/board-profile.md) follows at `+0x50`.

## Node (4 MB, `node_updater/partitions.csv`)

The existing 4 MB node layout is unchanged. Its `factory` holds the updater; `ota_0`/`ota_1` hold node apps. The SOLO ODU selects this table.

| Partition | Offset | Size | Contents |
|---|---|---|---|
| bootloader | `0x0` | — | ESP-IDF 2nd stage + recovery selection |
| partition table | `0x8000` | 4 KB | `node-4MB` table |
| `nvs` | `0x9000` | `0x5000` (20 KB) | application data |
| `otadata` | `0xE000` | `0x2000` (8 KB) | erased = boot `factory` |
| `phy_init` | `0x10000` | `0x1000` | |
| `cancfg` | `0x11000` | `0x1000` | identity, IDs, profile; subtype `0x40` |
| `bootcnt` | `0x12000` | `0x1000` | recovery log; subtype `0x42` |
| `factory` | `0x20000` | `0x60000` (384 KB) | CAN updater |
| `ota_0` | `0x80000` | `0x1C0000` (1.75 MiB) | app slot A |
| `ota_1` | `0x240000` | `0x1C0000` (1.75 MiB) | app slot B |

HELLO announces a 1,835,008-byte slot (896 windows of 2 KB). The update path never writes `factory`.

## IDU (16 MB, `node_updater/partitions_idu_16MB.csv`)

The SOLO IDU is both brain and node. Its app runs from `ota_0`/`ota_1`, handles its own node command/reply pair, and can stage other node images and packages. The shared updater sits at the **end** of flash, not at `0x20000`.

| Partition | Offset | Size | Contents |
|---|---|---|---|
| bootloader | `0x0` | — | same recovery bootloader, header patched for 16 MB |
| partition table | `0x8000` | 4 KB | `idu-16MB` table |
| `nvs` | `0x9000` | `0x5000` (20 KB) | application/package data |
| `otadata` | `0xE000` | `0x2000` (8 KB) | erased = boot `factory` |
| `phy_init` | `0x10000` | `0x1000` | |
| `cancfg` | `0x11000` | `0x1000` | identity, IDs, profile; subtype `0x40` |
| `bootcnt` | `0x12000` | `0x1000` | recovery log; subtype `0x42` |
| `ota_0` | `0x20000` | `0x300000` (3 MiB) | IDU app slot A |
| `ota_1` | `0x320000` | `0x300000` (3 MiB) | IDU app slot B |
| `node_fw` | `0x620000` | `0x200000` (2 MiB) | staged node image; subtype `0x41` |
| `pkg` | `0x820000` | `0x780000` (7.5 MiB) | package staging; subtype `0x43` |
| `factory` | `0xFA0000` | `0x60000` (384 KB) | CAN updater |

An IDU updater with no app flashed boots from `factory` and enumerates on USB as `SOLO IDU Controller`. The profile holds GPIO3 LOW from the 2nd-stage bootloader onward; ROM boot happens earlier and may leave it floating.

## Brain example (16 MB, `brain/partitions.csv`)

The separate `brain/` ESP-IDF example retains its **existing** A/B table. It has no `factory`, `cancfg` or `bootcnt`; it is not the SOLO IDU universal-bundle layout. `nvs`, `otadata`, `phy_init`, both 3 MiB OTA slots and the 2 MiB `node_fw` staging partition use the same offsets as the IDU table. Its physical `pkg` region begins at `0x820000` and extends to the end of 16 MB flash (8,257,536 bytes). The **accepted package maximum is still only `0x780000`**, checked by the package parser, builder and HTTP handler. The physical partition has more room than the supported package format allows. USB-install a new table before relying on partitions an older table lacks; `/ota` cannot install a table.

## Package limits

The complete `.ecbpkg` including manifest, gaps and payloads is limited to **`0x780000` = 7,864,320 bytes (7.5 MiB)** for either 16 MB staging layout. Each BRAIN payload is at most 3,145,728 bytes, and each NODE payload at most 1,835,008 bytes; eight maximum-size nodes plus a maximum-size brain do not fit. Packages do not replace a bootloader, updater or partition table. See [package format](../reference/package-format.md#esp32-s3-payloads-and-capacity).
