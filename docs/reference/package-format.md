# Update package format

Version 1. File extension: `.ecbpkg`. History: #10.
All integers are **unsigned little endian**. No packed C structs are cast onto
input bytes. This format provides corruption detection, **not authentication**:
there are no signatures, credentials or anti-rollback versions. Use a trusted LAN.

## Header (32 bytes)

| Offset | Bytes | Field | v1 value / meaning |
|---:|---:|---|---|
| 0 | 8 | magic | `45 43 42 50 4B 47 00 00` (`ECBPKG\0\0`) |
| 8 | 2 | format_version | 1 |
| 10 | 2 | header_size | 32 |
| 12 | 2 | entry_size | 96 |
| 14 | 2 | entry_count | 1..9, subject to per-type limits below |
| 16 | 4 | manifest_size | `32 + 96 * entry_count`, includes this header |
| 20 | 4 | total_size | exact file / HTTP Content-Length, not partition capacity |
| 24 | 4 | check | IEEE CRC32 of bytes `[0, manifest_size)`, treating these four bytes as zero |
| 28 | 4 | flags | zero; all bits reserved |

CRC32 is the reflected IEEE/zlib algorithm: polynomial `0xEDB88320`, initial
register `0xFFFFFFFF`, final XOR `0xFFFFFFFF`. The existing `crc32_ieee` and
Python `zlib.crc32` agree. The table starts immediately at byte 32. The largest
manifest is 896 bytes. A parser only needs this bounded manifest in RAM, plus
the actual stored file size supplied by the caller.

## Entry (96 bytes)

Offsets here are relative to the start of each entry.

| Offset | Bytes | Field | Meaning |
|---:|---:|---|---|
| 0 | 1 | type | 1 = BRAIN; 2 = NODE |
| 1 | 1 | flags | zero; reserved |
| 2 | 1 | name_length | 0..32; BRAIN requires zero |
| 3 | 1 | reserved | zero |
| 4 | 4 | payload_offset | absolute offset from start of file |
| 8 | 4 | payload_size | exact nonzero byte length |
| 12 | 2 | cmd_id | NODE standard 11-bit CAN command ID; BRAIN requires zero |
| 14 | 2 | reply_id | NODE standard 11-bit CAN reply ID; BRAIN requires zero |
| 16 | 32 | sha256 | SHA-256 of exactly `payload_size` payload bytes, including any ESP appended digest |
| 48 | 32 | product_name | `name_length` ASCII bytes, then zero padding |
| 80 | 16 | reserved | all zero |

There may be **zero or one BRAIN** and **zero to eight NODE** entries, but at
least one entry overall. Thus node-only and brain-only packages are valid; when
nodes are present their count is 1..8. Multiple BRAIN entries are invalid.
Unknown types, versions, flags, nonzero reserved bytes, invalid counts, and
noncanonical header/table lengths are rejected, not silently ignored.

For NODE entries both IDs are explicit values `0..0x7FF`. The builder defaults
them to `0x79E/0x79F`; zero is a real ID, not a default sentinel. Names may contain
ASCII bytes `0x01..0x7F`, not NUL; practical product names should be printable.
An empty name disables the check. Otherwise the name **and its length** must
exactly match the node's INFO response. Case is significant. Names are not device
serial numbers: provision unique command/reply IDs on a shared bus. The format
does not prohibit repeated IDs; they cause sequential updates of that address.

Every payload starts at a **four-byte-aligned** offset at or after
`manifest_size`, is nonempty, and lies wholly within `total_size`. Payload ranges
must not overlap each other or the manifest. Sizes need not be multiples of four.
Offsets need not follow table order; relay always follows NODE **table order**.
Gaps are permitted. The builder zero-fills alignment gaps; gap bytes have no
meaning and are not covered by a payload digest. Flip a manifest or payload byte,
not a gap, when testing corruption detection.

`src/boot/update_pkg.c` performs bytewise, overflow-safe manifest/range validation
without touching payload bytes, flash or peripherals. The brain computes payload
SHA-256 from flash with mbedtls; the Python `info` command computes it from the file.
Only use parsed entries when the C parser returns `UPDATE_PKG_OK`.

## ESP32-S3 payloads and capacity

The current caller/builder accepts **unsigned application `.bin` files**, not
bootloaders, merged flash dumps, or padded/signed containers. It checks magic
`0xE9`, ESP32-S3 chip ID **9** at header offset 12, 1..16 segments, the app descriptor
magic `0xABCD5432` at offset 32, segment bounds/alignment, and exact image length
(segment bytes, checksum/padding to 16 bytes, and optional 32-byte appended hash).
These are envelope checks; ESP-IDF's `esp_ota_end` performs full executable image
validation in the destination slot before boot selection. A structurally plausible
but internally invalid image can therefore still fail the brain step or a node's
VERIFY after the package has been accepted. No node relay follows a brain failure.

Use a **package-capable `brain_ota`** (Phase-5-capable or later) for BRAIN and the intended node app for NODE.
A valid S3 app cannot be identified as the right board firmware from its chip ID.
A BRAIN image without this package-resume code cannot continue the pending relay;
a self-confirming app without Ethernet can require USB recovery.

The new `pkg` partition is data subtype `0x43`, offset `0x820000`, size
**`0x7E0000` = 8,257,536 bytes = 7.875 MiB**, ending at `0x1000000`.
No existing brain partition moves:

| Label | Offset | Size |
|---|---:|---:|
| nvs | `0x9000` | `0x5000` |
| otadata | `0xE000` | `0x2000` |
| phy_init | `0x10000` | `0x1000` |
| ota_0 | `0x20000` | `0x300000` (3,145,728 bytes) |
| ota_1 | `0x320000` | `0x300000` (3,145,728 bytes) |
| node_fw | `0x620000` | `0x200000` |
| pkg | `0x820000` | `0x7E0000` |

Each NODE payload is at most **`0x1C0000` = 1,835,008 bytes**; each BRAIN payload
must fit a 3 MiB OTA slot. The **entire package** (header, table, gaps, all payloads)
must fit `pkg`: one maximum-size brain plus eight maximum-size nodes will not fit.
With nine entries the payload budget is at most **8,256,640 bytes**, minus gaps.
The tool and brain enforce these limits. Install the new partition table over
USB once; `/ota` and `/package` do not update partition tables.

## Brain-first transaction and reboot record

All routes that scan, reset or update share one static worker. Both status routes
remain independent. Startup reserves that worker until the existing Ethernet
self-test finishes and the pending-record check has run; conflicting jobs receive
409 even during startup. Package staging streams to `pkg` sector by sector,
read-back-verifies each write, validates the manifest and **every** payload SHA
and slot limit, then responds **202**. No OTA-slot, NVS pending-record, node-reset
or CAN mutation happens on an invalid package. Partial/invalid staging may of
course overwrite `pkg` itself.

The worker compares the BRAIN entry against raw running-partition bytes over the
canonical image length reported by `esp_image_get_metadata`. It does **not** use
`esp_partition_get_sha256`: for an app with an appended SHA, that API returns the
embedded digest, which excludes the digest itself and differs from a whole `.bin`
hash. Length and raw SHA are both compared. Matching BRAIN is `skipped_same`;
absent BRAIN is `none`.

A changed BRAIN is written with `esp_ota_begin/write/end` into the inactive slot,
then SHA-256 checked again from that slot. Before selecting it, the worker commits
an 80-byte NVS blob, namespace `package`, key `pending`:

| Offset | Bytes | Value |
|---:|---:|---|
| 0 | 4 | record version = 1 |
| 4 | 4 | package total size |
| 8 | 4 | expected running brain's canonical image size |
| 12 | 4 | brain outcome: 0 none, 1 skipped_same, 2 updated |
| 16 | 32 | SHA-256 of the stored header + table, including its CRC field |
| 48 | 32 | expected raw brain image SHA-256 |

For node-only/same-brain jobs, the record names the current running image and is
committed before the first node probe. For changed-brain jobs, it names the new
image, then `esp_ota_set_boot_partition` selects that verified slot and the brain
reboots. The old image remains the rollback candidate.

On boot, after the existing self-test, the worker reads the record:

- Running length/SHA differs: report `failed`, brain `rolled_back`, exact text
  **`brain update rolled back; node relay aborted`**, clear the record, send no
  CAN traffic and no recovery pulses. Safe node identities are recovered from
  the checked manifest for display, with skipped outcomes.
- Expected image matches but is not **VALID**: retain the record, block new jobs,
  do not relay. This includes the SKIP_MARK_VALID test build; the next reset
  triggers normal ESP-IDF rollback. It is not safe to relay merely because the
  hash matches an unconfirmed image.
- Expected image matches and is VALID: revalidate the stored package identity,
  all payload hashes and limits, rebuild node status, relay from the first NODE.
  Corrupt/stale package contents fail closed and clear the pending record.

Each node is first sent ENTER_UPDATER on its own IDs (up to three 0.5 s
attempts): a running app with the CAN service ACKs and restarts into its updater
(the brain then waits 1.5 s), and an updater NAKs it at once. After either reply
the node is probed with INFO. If ENTER_UPDATER gets no answer, or INFO then times
out or transport cannot send (e.g. the app has CAN stopped and cannot ACK), run
the recovery pulse train ([recovery.md](../concepts/recovery.md)), wait 1 s, then
retry INFO. **Only the node wired to GPIO3 can be
recovered this way.** Looking for an absent/unwired node can therefore reset the
wired node; plan the bus and manifest order accordingly. Other nodes must already
be parked. A name mismatch skips flashing. Otherwise `can_flash_host_flash` reads
the entry directly from `pkg`, preserving the existing INFO/HELLO/SIZE/ERASE/DATA/
VERIFY/GO implementation and diagnostics. Failures are recorded and the next node
is attempted. `ok` means VERIFY and GO were ACKed, not that the new app later
confirmed itself. A lost GO ACK remains ambiguous, not success.

At completion the pending key is erased and committed. Status becomes `done`,
including when some nodes failed/skipped (inspect each result). It remains in RAM
until the next request/reset. **Brain power loss during relay restarts from the
first node**, not the interrupted node; earlier successful nodes can be reflashed.
Unwired nodes already running their apps may then need manual parking.

NVS init/commit errors do not erase NVS or silently abandon a possibly retained
job. Unknown/damaged records or failure to clear the key block new mutations and
report a service/reboot-required failure. Status stays readable. Do not replace
`pkg` behind a retained job.

## HTTP status

Routes, status codes and the `GET /package/status` JSON schema (states,
`brain` outcomes, node states and result texts):
[brain-http-api.md](brain-http-api.md#post-package).

## Power-loss table

| Flash mutation / interruption point | Result on next boot |
|---|---|
| `pkg` erase/write/read-back or validation, before acceptance | No pending key, active images unchanged; partial pkg is never auto-relayed. Re-upload. |
| Brain inactive-slot erase/write/end/read-back, before pending commit | Old brain boots, nodes untouched. Incomplete inactive image is not selected. Re-upload. |
| Pending NVS write/commit before boot selection | Either no record, or a complete record. With a new-brain record but old brain still running, conservative rollback/abort result; no nodes. |
| Brain boot-selection write / reboot | ESP-IDF redundant otadata chooses old or verified new image. Old image aborts relay; new image must pass self-test and revalidate pkg first. |
| Brain self-test / mark-valid | Pending image rolls back on reset until marked valid; old image aborts relay. After valid, resume can relay. |
| Pending commit for same/no-brain job | No record means no automatic relay; complete record means full revalidation and replay. |
| Node erase/write/VERIFY/GO | Existing node A/B/updater guarantees apply. On brain reboot, revalidate and restart from NODE 1; GO may already have succeeded. |
| Pending-key erase/commit after relay | Key absent: no replay, status idle after next boot. Key retained: safe but potentially duplicate full relay. |

Tests and exact bench procedures: [bench/update-package.md](../testing/bench/update-package.md).
