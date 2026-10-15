# Build & Flash Guide — ESP32-S3 CAN Updater

## One command

```sh
./build.sh                                              # Linux / macOS
powershell -ExecutionPolicy Bypass -File build.ps1      # Windows (PowerShell 5.1 or 7)
```

**Requirements:** Git, and Python 3.8–3.13 (on Windows: python.org, the `py`
launcher, or a uv-managed interpreter; on Debian/Ubuntu also `python3-venv`).
The script checks both up front and names whichever is missing.

What the first run does (later runs skip straight to the build):

1. Clones ESP-IDF at the exact tag in **`idf-version.txt`** (v5.3.6) into
   `.tools/esp-idf` (shallow, with submodules, ~1 GB).
2. Runs ESP-IDF's own installer for **esp32s3 only**, with `IDF_TOOLS_PATH`
   pointed at `.tools/espressif`: Xtensa/RISC-V toolchains, CMake, Ninja and
   a Python virtualenv. pip and ccache caches also stay under `.tools/`, so
   nothing is written to your user profile. `.tools/` is git-ignored; delete
   it to start over.
3. Exports that environment and runs `idf.py set-target esp32s3` (only when
   there is no `sdkconfig` yet) and `idf.py build`.
4. Prints every binary with its flash offset.

**Checkout path with spaces.** The ESP-IDF toolchain cannot run from a path
containing spaces (CMake passes such a compiler path to Ninja as an 8.3 short
name, which the `xtensa-esp32s3-elf-gcc` wrapper does not recognise). On
Windows, `build.ps1` therefore reaches the same repo-local `.tools` through a
directory junction at `%LOCALAPPDATA%\esp-can-updater-tools\<hash>` — a link
only, holding no data; delete it any time. `build.sh` stops with a clear
message instead: clone into a path without spaces on Linux/macOS.

Any arguments are passed to `idf.py` instead of `build`, inside the same
repo-local environment:

```sh
./build.sh menuconfig                    # Kconfig (pins, bitrate, ...)
./build.sh -p /dev/ttyUSB0 flash monitor # first flash over USB/UART
powershell -ExecutionPolicy Bypass -File build.ps1 -p COM5 flash monitor
```

**Another project.** A first argument naming an ESP-IDF project directory
(relative to the repo root) builds that project instead, with the same
repo-local ESP-IDF. Without further arguments it runs `idf.py app`, since an
application only needs its OTA image, and prints that image's path. Any
remaining arguments go to `idf.py` as above:

```sh
./build.sh examples/bench_app                                # -> examples/bench_app/build/bench_app.bin
powershell -ExecutionPolicy Bypass -File build.ps1 examples/bench_app
powershell -ExecutionPolicy Bypass -File build.ps1 examples/bench_app menuconfig
```

(It is positional rather than a `-Project` switch, because PowerShell would
bind idf.py's `-p` port option to `-Project` by prefix.) `build`, `size` or
`flash` on an application also builds its own bootloader, which is never
used. In a deep Windows checkout, that sub-project's object paths exceed
`MAX_PATH` and CMake warns, so stay with the default `app`. The bench-test app
and its rollback-test variant are described in
**[bench-test.md](bench-test.md)**.

**The SLCAN bridge is the exception.** `examples/slcan_bridge` runs on the
brain as a USB-CAN adapter (see below). It is not an OTA image, so it is
flashed over USB with its own bootloader and partition table, and the
scripts' "OTA image to flash over CAN" message does not apply to it:

```sh
powershell -ExecutionPolicy Bypass -File build.ps1 examples/slcan_bridge -p COM8 flash
./build.sh examples/slcan_bridge -p /dev/ttyACM1 flash
```

Its bootloader sub-project triggers the same CMake path-length warning in a
deep Windows checkout; the build still completes.

`examples/brain_ota` (the brain updating itself over Ethernet, issue #5) is
built and first flashed the same way, with its own 16 MB partition table;
after that it takes new images over HTTP. See
[bench-test.md, section 8](bench-test.md#8-brain-ota-over-ethernet-issue-5).

Changing the pin in `idf-version.txt` makes the next run replace
`.tools/esp-idf` with the new tag. CI (`.github/workflows/esp32-build.yml`)
runs the same `build.sh`, so CI and local builds always use the same ESP-IDF.

### Already have ESP-IDF v5.3, or prefer Docker

```sh
idf.py set-target esp32s3 build          # in an exported ESP-IDF v5.3 shell

docker run --rm -v "$PWD:/project" -w /project espressif/idf:v5.3.6 \
    idf.py set-target esp32s3 build
```

A `build/` configured in Docker (`/project`) is discarded automatically by
the scripts the next time they run natively; it is only build output.

## Outputs

| Offset | File | What |
|---|---|---|
| `0x0` | `build/bootloader/bootloader.bin` | stock 2nd-stage bootloader (app rollback) |
| `0x8000` | `build/partition_table/partition-table.bin` | `partitions.csv` |
| `0xE000` | `build/ota_data_initial.bin` | empty `otadata` → boots `factory` |
| `0x20000` | `build/can_updater.bin` | the CAN updater (`factory`) |

The offsets come from `build/flasher_args.json`; the scripts print them from
there.

## First flash of a board (USB/UART, once per board)

The updater, bootloader and partition table go on over the S3's USB-Serial-JTAG
or UART0 with esptool. After that, applications arrive over CAN.

```sh
./build.sh -p /dev/ttyUSB0 flash
# or, from build/ in an environment with esptool:
python -m esptool --chip esp32s3 write_flash @flash_args
```

The same image serves both boards: ESP32-S3-WROOM-1U-N16R8 (16 MB flash; the
layout uses the first 4 MB) and ESP32-S3FH4R2 (4 MB). The updater leaves PSRAM
unconfigured, so octal vs. quad PSRAM does not matter to it.

## Configuration (menuconfig)

`./build.sh menuconfig` → **CAN Updater**:

| Option | Default | Notes |
|---|---|---|
| `CAN_UPDATER_TWAI_TX_GPIO` | 4 | to the transceiver's TXD (TCAN332 `D`) |
| `CAN_UPDATER_TWAI_RX_GPIO` | 5 | from the transceiver's RXD (TCAN332 `R`) |
| `CAN_UPDATER_BITRATE` | 250 kbit/s | 125 k / 250 k / 500 k / 1 M; must match `can_flash.py --bitrate` |

TWAI goes through the GPIO matrix, so any free GPIO works. Avoid the strapping
pins (0, 3, 45, 46), USB (19, 20), flash/PSRAM pins (26–37 on octal-PSRAM
modules such as the N16R8) and UART0 (43, 44).

The defaults match the node board (ESP32-S3FH4R2 + TCAN332DR, issue #2).

**No recovery pin.** ESP-IDF's GPIO factory reset
(`CONFIG_BOOTLOADER_FACTORY_RESET`) is off: GPIO4 is the node's TWAI TX, and
the bootloader would sample the transceiver's D line at reset. Software
recovery (an RTC crash-loop counter, plus a flash boot counter the brain can
trip with EN pulses) arrives in Phase 3. Until then the ways into the updater
are: a fresh board, the app calling `can_updater_client_enter_updater()`, or
rollback of an app that never marks itself valid. On the bench, erasing
`otadata` over USB (`python -m esptool --chip esp32s3 erase_region 0xe000
0x2000`) also brings the updater back.

To keep a board-specific configuration in git, put the overrides in
`sdkconfig.defaults` (the generated `sdkconfig` is ignored).

## Wiring a transceiver

```
ESP32-S3                 3.3 V CAN transceiver           bus
TX GPIO (4) ───────────► TXD                 CANH ───────
RX GPIO (5) ◄─────────── RXD                 CANL ───────
3V3 ──────────────────── VCC (and VIO)                 120 Ω at both ends
GND ──────────────────── GND ─── common ground with the host adapter
```

Use a 3.3 V part (SN65HVD230, TCAN332, MCP2562 with VIO = 3.3 V). Tie any
standby/silent pin to its active level. To test a wired board with a PC and
a USB-CAN adapter, follow **[bench-test.md](bench-test.md)**.

## SLCAN bridge on the brain (`examples/slcan_bridge`)

Turns the brain (ESP32-S3-WROOM-1U-N16R8 + TCAN332DR) into an SLCAN
(Lawicel) adapter on its native USB-Serial-JTAG port, so
`can_flash.py --interface slcan --channel COMx` reaches a node with no other
CAN hardware ([bench-test.md](bench-test.md)). `build.ps1
examples/slcan_bridge menuconfig` → **SLCAN Bridge**:

| Option | Default | Notes |
|---|---|---|
| `SLCAN_BRIDGE_TWAI_TX_GPIO` | 6 | to the brain transceiver's TXD (TCAN332 `D`) |
| `SLCAN_BRIDGE_TWAI_RX_GPIO` | 7 | from its RXD (TCAN332 `R`) |
| `SLCAN_BRIDGE_BITRATE` | 250 kbit/s | used by an `O` without a prior `Sn`; python-can sends `Sn` |

- **Commands:** exactly python-can's `slcan` subset (`components/slcan_codec`):
  `C`, `O`, `L` (listen-only), `S0`–`S8`, `t`/`T`/`r`/`R`, `V`, `N`;
  received frames go out as `t`/`T`/`r`/`R` lines. `sXXYY`, `S9` and CAN FD
  are NAKed, and so is an `O` after `S7`: the 80 MHz TWAI clock cannot make
  750 kbit/s.
- **The USB port carries SLCAN only.** Logs and panics go to UART0
  (GPIO43/44, 115200): `CONFIG_ESP_CONSOLE_SECONDARY_NONE` in its
  `sdkconfig.defaults`. Bus-state changes (error-passive, bus-off and its
  automatic recovery) and lost frames are logged there.
- **Buffering:** a 16 KB USB RX ring (a 342-frame can-flash window is
  7.5 KB of SLCAN; the USB-Serial-JTAG driver drops host bytes on overflow
  rather than stalling USB), an 8 KB USB TX ring and a 256-frame TWAI RX
  queue.
- **GPIO3** (the node's reset, inverted: HIGH holds the node in reset) is
  driven LOW from boot so the node always runs.

## Per-device CAN IDs and INFO name (optional)

Unprovisioned boards use `0x79E` (host→board) / `0x79F` (board→host) and
answer INFO with an empty name. To give a board its own IDs (several parked
boards on one bus) and a name for `--scan`, write the 80-byte v2 config block
(`src/boot/dfu_config.h`, same format as the RA4M1) to the `cancfg`
partition:

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
faults on it.

## Integrating an application

An application that should be updatable over CAN (`examples/bench_app/` is a
minimal working example):

1. **Use the updater's layout.** Copy `partitions.csv` and set, in the app's
   `sdkconfig.defaults`:

   ```
   CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
   CONFIG_PARTITION_TABLE_CUSTOM=y
   CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
   CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
   ```

2. **Add the client component.** Point the app at it (or copy the folder):

   ```cmake
   # app CMakeLists.txt, before include(project.cmake)
   set(EXTRA_COMPONENT_DIRS "<this repo>/components/can_updater_client")
   ```

   and `REQUIRES can_updater_client` in the component that handles CAN.

3. **Enter the updater on request.** Where the app handles its "enter
   bootloader" request (on the Airform boards: the `/enter_bootloader` route
   that `can_flash.py --enter-bootloader` sends over ISO-TP):

   ```c
   #include "can_updater_client.h"

   static void on_enter_bootloader(void)
   {
       esp_err_t err = can_updater_client_enter_updater();  /* restarts on success */
       ESP_LOGE(TAG, "cannot enter the CAN updater: %s", esp_err_to_name(err));
   }
   ```

   It selects `factory` and restarts; `esp_restart()` resets the peripherals,
   so the app's own TWAI driver needs no teardown. The board then stays in
   the updater (across resets too) until a new image is flashed.

4. **Confirm the image.** Once the app is healthy, call
   `esp_ota_mark_app_valid_cancel_rollback()`. An app that never does is
   rolled back on its next reset, which lands the board in the updater.

5. **Build and flash it over CAN** — `build/<app>.bin` is the OTA image:

   ```sh
   uv run tools/can-flash/can_flash.py path/to/app/build/my_app.bin \
       --interface socketcan --channel can0 --bitrate 250000 \
       --enter-bootloader                      # if the app is running
       # --id-tx 0x700 --id-rx 0x711           # if the board has its own IDs
   ```

Do **not** `idf.py flash` an application onto an updater board: it would
rewrite the bootloader with the app's configuration and put the app into the
first app partition, which is `factory` — overwriting the updater. Deliver
applications over CAN (or with esptool to `ota_0` at `0x80000` plus a matching
`otadata`).

## Host tests

```sh
cmake -S tests -B build-host && cmake --build build-host && ctest --test-dir build-host -C Debug --output-on-failure
```

`-C Debug` is needed with multi-config generators (the Visual Studio default
on Windows) and ignored by single-config ones. They cover the shared
pure-logic core in `src/boot/` (protocol engine, CRC32,
config-block parser, and the RA4M1 image validator kept as prior art) and
the SLCAN codec in `components/slcan_codec/`. No ESP-IDF needed.
