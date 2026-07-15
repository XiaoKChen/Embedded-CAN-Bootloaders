# Building & Flashing the UNO R4 Minima DFU+CAN Bootloader

The bootloader is the stock Arduino UNO R4 bootloader (USB DFU, TinyUSB)
forked in-tree with one added feature: flash-over-CAN on D4/D5, per
`docs/research/r4-minima-can-bootloader.md`. USB DFU behavior, USB PID and
the memory map are unchanged; the Arduino IDE upload workflow keeps working.

## Vendored components (exact versions)

| Component | Path | Version |
|---|---|---|
| arduino-renesas-bootloader | `vendor/arduino-renesas-bootloader/` | commit `581a59d0d9aa8e9ad21a43d62ea6fd063bc5fe19` (upstream `main`, 2026-05-12) + this repo's CAN integration (edits to `src/main.c`, `Makefile.minima`) |
| tinyusb | `vendor/tinyusb/` | tag `0.17.0` (`5217cee5de4cd555018da90f9f1bcc87fb1c1d3a`) with the fork's `0001-fix-arduino-bootloaders.patch` applied |
| renesas/fsp (tinyusb dep) | `vendor/tinyusb/hw/mcu/renesas/fsp/` | `d52e5a6a59b7c638da860c2bb309b6e78e752ff8` (pin from tinyusb `tools/get_deps.py`) |
| ARM CMSIS_5 (tinyusb dep) | `vendor/tinyusb/lib/CMSIS_5/` | `20285262657d1b482d132d20d755c8c330d55c1f` (same pin source) |
| RA4M1 device headers | `vendor/ra4m1-headers/` | self-contained set used ONLY by `src/drivers/can_polled.c` (one device-header source per translation unit) |

> **After cloning this repo you MUST fetch the tinyusb deps before building.**
> `vendor/tinyusb/lib/CMSIS_5/` and `vendor/tinyusb/hw/mcu/renesas/fsp/` are
> `.gitignore`d (they are ~280 MB of mostly-unused upstream source). The
> patched tinyusb core *is* committed; only the two deps need restoring:

```sh
cd vendor/tinyusb
# deps (equivalent to `python3 tools/get_deps.py ra`):
git -C hw/mcu/renesas/fsp init && git -C hw/mcu/renesas/fsp fetch --depth 1 \
    https://github.com/renesas/fsp.git d52e5a6a59b7c638da860c2bb309b6e78e752ff8 \
    && git -C hw/mcu/renesas/fsp checkout FETCH_HEAD
git -C lib/CMSIS_5 init && git -C lib/CMSIS_5 fetch --depth 1 \
    https://github.com/ARM-software/CMSIS_5.git 20285262657d1b482d132d20d755c8c330d55c1f \
    && git -C lib/CMSIS_5 checkout FETCH_HEAD
```

To re-vendor tinyusb itself from scratch (only needed if the committed core is
lost):

```sh
cd vendor
git clone --depth 1 --branch 0.17.0 https://github.com/hathach/tinyusb
cd tinyusb
patch -p1 < ../arduino-renesas-bootloader/0001-fix-arduino-bootloaders.patch
# then run the two dep-fetch commands above.
```

## Prerequisites

- **Cross toolchain:** GNU Arm Embedded **gcc 14.2.1** (xPack build). Download
  and extract to a **short path** (a deeply nested path makes gcc fail with
  `cannot execute 'cc1': CreateProcess: No such file or directory` on Windows):

  <https://github.com/xpack-dev-tools/arm-none-eabi-gcc-xpack/releases/tag/v14.2.1-1.1>
  (`xpack-arm-none-eabi-gcc-14.2.1-1.1-win32-x64.zip`), e.g. to
  `C:\tools\gcc14\`. Point the build at it with `CROSS_COMPILE` (below), or put
  its `bin\` on `PATH`.

  The older Arduino-bundled gcc 7.2.1
  (`%LOCALAPPDATA%\Arduino15\packages\arduino\tools\arm-none-eabi-gcc\7-2017q4\bin`)
  still builds, but produces a ~648 B larger image and needs the
  `CFLAGS_SKIP` workaround noted below. Prefer gcc 14.
- **GNU make + POSIX sh** (the tinyusb build system is Make/sh based). On
  Windows, the xPack *windows-build-tools* bundle provides both
  (`make.exe`, busybox `sh.exe`):
  <https://github.com/xpack-dev-tools/windows-build-tools-xpack/releases>
- **A space-free checkout path.** GNU make cannot handle paths with spaces.
  If the repo lives at a spaced path (as here), map a drive letter:

  ```sh
  subst B: "C:\Users\XiaokangChen\Documents\Programming\Work ( Normal Corporation )\Arduino-Bootloader"
  ```

  and build from `B:\`. (Alternative: WSL with a Linux arm-none-eabi
  toolchain and a `~/arduino-bootloader` symlink — not the path used here.)

## Build (direct Make)

From Git Bash:

```sh
export PATH="/path/to/xpack-windows-build-tools/bin:$PATH"

cd /b/vendor/arduino-renesas-bootloader
make -f Makefile.minima BOARD=uno_r4 \
     TOP=../tinyusb CURRENT_PATH=. CMDEXE= \
     CROSS_COMPILE="C:/tools/gcc14/bin/arm-none-eabi-" \
     BOOTLOADER_SHELL="/path/to/xpack-windows-build-tools/bin/sh.exe" \
     -j8
```

Windows-specific arguments, all no-ops on a Linux host (where plain
`TINYUSB_ROOT=... make -f Makefile.minima` works):

- `CROSS_COMPILE=<abs path>/arm-none-eabi-` — selects the gcc 14 toolchain
  without putting it on `PATH`. Use a **short** install path (see
  Prerequisites re: the `cc1 CreateProcess` failure). Omit if the toolchain's
  `bin\` is already on `PATH`.
- `TOP=../tinyusb CURRENT_PATH=.` — keeps every path relative; absolute
  Windows paths ("`B:`") contain a colon, which breaks make target names.
- `CMDEXE=` + `BOOTLOADER_SHELL=<abs path to sh.exe>` — tinyusb's `make.mk`
  latches `SHELL := cmd.exe` when `PATH` contains a semicolon; the
  escaped-space `-D` defines require a POSIX sh. The path must be absolute,
  space-free, and set via `BOOTLOADER_SHELL` (a makefile-level assignment —
  GNU make on Windows ignores a command-line `SHELL` once cmd.exe is latched).
- `CFLAGS_SKIP="-Wcast-function-type"` — **gcc 7.x only** (it does not know
  this warning flag, which tinyusb adds for gcc >= 8). Do **not** pass it with
  gcc 14.

Artifacts land in `_build/uno_r4/`:
`arduino-renesas-bootloader.{elf,bin,hex}` + `.elf.map`.

### Size gate

The build fails if the `.bin` exceeds the 16 384-byte bootloader region
(8 × 2 KB blocks, `src/boot/memory_map.h` `BOOT_SIZE`). Current output with
gcc 14.2.1:

```
SIZE GATE OK: 15056 / 16384 bytes
```

That leaves **1 328 B** of headroom. For reference: the same source built with
the old gcc 7.2.1 is 15 704 B (648 B larger, only 680 B of headroom), which is
why gcc 14 is preferred. Arduino's shipped stock hex (gcc unknown) is a
12 424 B binary; the CAN update path is the difference.

The `.hex` file on disk is ~2.8× the `.bin` size (Intel HEX is ASCII text);
only the `.bin` figure is what the chip stores and what the size gate checks.

## Build (CMake wrapper)

```sh
cmake -B build-host \
      -DBOOTLOADER_SHELL=<abs path to sh.exe> \
      -DBOOTLOADER_SOURCE_DIR=B:/vendor/arduino-renesas-bootloader
cmake --build build-host --target bootloader     # firmware (wraps make)
cmake --build build-host                          # host test binaries
ctest --test-dir build-host -C Debug --output-on-failure
```

The `bootloader` target shells out to the Make build above (tinyusb is not
ported to CMake); the toolchain/make/sh must be on `PATH` when invoking it.

## Pre-flash check: option bytes (risk R4)

OFS0/OFS1 live at flash `0x400`/`0x404` (Security-MPU words follow through
`0x43F`) — inside the bootloader image. Wrong values can auto-start the IWDT
or lock the part. Before first flash, confirm bytes `0x400–0x43F` of the
built image match the stock bootloader (`docs/reference/dfu_minima.hex`):

```sh
od -A x -t x1 -j 0x400 -N 0x40 _build/uno_r4/arduino-renesas-bootloader.bin
```

Expected (verified identical to stock hex records `:10040000`–`:10043000`):

```
000400 ff ff ff ff df ce ff ff fc ff ff 00 ff ff ff 00
000410 fc ff ff 00 ff ff ff 00 fc ff ff 00 ff ff ff 00
000420 fc ff 0f 20 ff ff 0f 20 fc ff 7f 40 ff ff 7f 40
000430 fc ff 0d 40 ff ff 0d 40 ff ff ff ff ff ff ff ff
```

(OFS0 = `0xFFFFFFFF` — IWDT/WDT auto-start disabled; OFS1 = `0xFFFFCEDF`.)
These come from the fork's linker sections — do not edit them.

## Flashing

Replacing the bootloader is the only risky flash operation; the RA4M1's
mask-ROM boot and SWD always recover it (unbrickable at chip level as long
as the ID code stays `0xFF`, which this build keeps).

1. **ROM boot mode + rfp-cli (no probe needed):** short the **BOOT pad to
   GND**, reset/replug USB → "Renesas RA USB Boot" (USB CDC) enumerates.
   ```
   rfp-cli -device ra -port COMx -p _build/uno_r4/arduino-renesas-bootloader.hex
   ```
   Keep BOOT–GND shorted until programming starts.
2. **SWD + pyOCD** (CMSIS-DAP or J-Link on the SWD pads):
   ```
   pyocd flash -e sector -a 0x0 -t r7fa4m1ab _build/uno_r4/arduino-renesas-bootloader.hex
   ```
3. Stock USB DFU **cannot** replace the bootloader (its descriptor marks
   `0x0000–0x3FFF` read-only) — use 1 or 2.

### Unbrick / restore factory bootloader

```
pyocd flash -e sector -a 0x0 -t r7fa4m1ab docs/reference/dfu_minima.hex
```

or the same hex via `rfp-cli` in ROM boot mode (path 1).

## CAN update path (what got integrated)

- Boot decision (`vendor/arduino-renesas-bootloader/src/main.c`, `CAN_LOADER`):
  1. `VBTBKR[0]` double-tap magic → USB DFU mode (stock, unchanged).
  2. `VBTBKR[4]` = `CAN_UPDATE_MAGIC` (`0xB007CAFE`, written by the app,
     PRC1-unlocked) → cleared → CAN-only update mode at 250 kbit/s on
     D4 (CTX0/P103) / D5 (CRX0/P102). External 5 V CAN transceiver required.
  3. App validation: CRC metadata block at `0x3F800` (see
     `src/boot/image_header.h`) is authoritative; metadata absent/corrupt
     falls back to the stock initial-SP check; metadata intact but image
     invalid stays in the bootloader — with **both** DFU and CAN alive.
- Protocol: `src/boot/can_update.h` (HELLO/SIZE/ERASE/DATA/VERIFY/GO on IDs
  `0x79E`/`0x79F`; host tool in `tools/can-flash/`).
- After a completed DFU download the bootloader now computes CRC32 over the
  received image and writes a fresh metadata block, so DFU-uploaded sketches
  are CRC-validated on later boots too (research §5.2, risk R7).
- Power loss at any instant leaves either a validated image (boots) or an
  invalid/absent metadata block (parks in the bootloader) — never a brick;
  the bootloader region is never self-written.
