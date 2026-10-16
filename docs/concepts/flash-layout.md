# Flash layout

## Node (4 MB, `node_updater/partitions.csv`)

One table serves every node OTA app (`examples/*` point at it) and both boards:
it fits 4 MB, and boards with more flash (WROOM-1U-N16R8) use the first 4 MB.
The updater does not use PSRAM.

| Partition | Offset | Size | Contents |
|---|---|---|---|
| bootloader | `0x0` | — | ESP-IDF 2nd stage + recovery selection (rollback enabled) |
| partition table | `0x8000` | 4 KB | |
| `nvs` | `0x9000` | 20 KB | application data |
| `otadata` | `0xE000` | 8 KB | which OTA slot boots (erased = boot `factory`) |
| `phy_init` | `0x10000` | 4 KB | |
| `cancfg` | `0x11000` | 4 KB | per-device CAN-ID / name block (optional, data subtype `0x40`), [provisioning](../guides/provisioning-can-ids.md) |
| `bootcnt` | `0x12000` | 4 KB | recovery bit log (data subtype `0x42`, the previously unused `0x12000..0x12fff`); no existing offset moves |
| `factory` | `0x20000` | 384 KB | **the CAN updater** (~50 % free) |
| `ota_0` / `ota_1` | `0x80000` / `0x240000` | 1.75 MB each | the application |

HELLO announces the **1.75 MB** slot: `0x1C0000` = 1,835,008 bytes (896
windows of 2 KB). Nothing in the update path writes `factory`.

## Brain (16 MB, `brain/partitions.csv`)

No factory app: the brain updates itself A/B between `ota_0` and `ota_1`, and
rollback falls back to the other slot.

| Partition | Offset | Size | Contents |
|---|---|---|---|
| bootloader | `0x0` | — | stock ESP-IDF 2nd stage, app rollback enabled |
| partition table | `0x8000` | 4 KB | |
| `nvs` | `0x9000` | `0x5000` (20 KB) | package pending record (namespace `package`) |
| `otadata` | `0xe000` | `0x2000` (8 KB) | boot-slot selection and image states |
| `phy_init` | `0x10000` | `0x1000` (4 KB) | |
| `ota_0` | `0x20000` | 3 MB | app slot A (the USB flash lands here) |
| `ota_1` | `0x320000` | 3 MB | app slot B |
| `node_fw` | `0x620000` | 2 MB | disposable staged node image (custom data subtype `0x41`) for `/node/flash` |
| `pkg` | `0x820000` | `0x7E0000` (7.875 MiB) | retained package staging, custom data subtype `0x43` |

`brain/partitions.csv` retains `nvs` at `0x9000`/`0x5000`, `otadata` at
`0xE000`/`0x2000`, `phy_init` at `0x10000`/`0x1000`, 3 MiB `ota_0` at
`0x20000`, 3 MiB `ota_1` at `0x320000`, and 2 MiB `node_fw` at `0x620000`.
The **`pkg`** partition uses the formerly free range `0x820000..0x1000000`,
size **`0x7E0000` / 8,257,536 bytes**, ending at `0x1000000`. Nothing moves;
all pre-existing offsets/sizes remain unchanged. USB-install the table once
before using `/package` (a normal `/ota` upload cannot install a table).

## Package limits

The complete package must fit `pkg`; per-image limits are 3,145,728 bytes for
BRAIN and 1,835,008 bytes for each NODE. Eight maximum-size nodes plus a
maximum-size brain cannot fit. Packages do not update the bootloader or
partition table. Details: [package-format.md](../reference/package-format.md#esp32-s3-payloads-and-capacity).
