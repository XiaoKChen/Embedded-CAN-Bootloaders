# Build

Every project on this branch builds with one script and a repo-local,
pinned ESP-IDF. First USB flash: [first-flash.md](first-flash.md). Kconfig
options and build variants: [configuration.md](../reference/configuration.md).

## One command

```sh
./build.sh                                              # Linux / macOS
powershell -ExecutionPolicy Bypass -File build.ps1      # Windows (PowerShell 5.1 or 7)
```

With no argument this builds the factory updater and the separate brain
firmware example, in this order:

| Project | Layout at build time | Output |
|---|---|---|
| `node_updater/` | 4 MB node; bundle also generates IDU 16 MB table | common bootloader/updater, both partition tables, legacy and universal updater zips |
| `brain/` | separate brain 16 MB table (no factory) | Ethernet OTA example `brain_ota.bin` (`ota_0`) |

The updater uses the managed ESP-IDF component `espressif/esp_tinyusb`
**1.7.6** for its CDC-ACM device. Its first build fetches this dependency
from the component registry, so a first build needs network access even when
ESP-IDF was already installed. Later builds use the cached component.
A failure in either project or in bundle generation stops the script.

**Requirements:** only **Git**, and **Python 3.8–3.13** (on Windows:
python.org, the `py` launcher, or a uv-managed interpreter; on Debian/Ubuntu
also `python3-venv`). The script checks both up front and names whichever is
missing.

What the first run does (later runs skip straight to the build):

1. Clones ESP-IDF at the exact tag in **`idf-version.txt`** (v5.3.6) into
   `.tools/esp-idf` (shallow, with submodules, ~1 GB).
2. Runs ESP-IDF's own installer for **esp32s3 only**, with `IDF_TOOLS_PATH`
   pointed at `.tools/espressif`: Xtensa/RISC-V toolchains, CMake, Ninja and
   a Python virtualenv. pip and ccache caches also stay under `.tools/`, so
   nothing is written to your user profile. `.tools/` is git-ignored; delete
   it to start over.
3. Exports that environment and, in each project, runs `idf.py set-target
   esp32s3` (only when there is no `sdkconfig` yet) and `idf.py build`.
4. Builds the updater (then generates two partition-table binaries and both
   updater bundles) and the separate brain example; prints each project's
   binary offsets.

Changing the pin in `idf-version.txt` makes the next run replace
`.tools/esp-idf` with the new tag. CI (`.github/workflows/esp32-build.yml`)
runs the same `build.sh`, so CI and local builds always use the same ESP-IDF.

### Checkout path with spaces

The ESP-IDF toolchain cannot run from a path containing spaces (CMake passes
such a compiler path to Ninja as an 8.3 short name, which the
`xtensa-esp32s3-elf-gcc` wrapper does not recognise). On Windows, `build.ps1`
therefore reaches the same repo-local `.tools` through a directory junction at
`%LOCALAPPDATA%\esp-can-updater-tools\<hash>` — a link only, holding no data;
delete it any time. `build.sh` stops with a clear message instead: clone into
a path without spaces on Linux/macOS.

## One project

A first argument naming an ESP-IDF project directory (relative to the repo
root: `node_updater`, `brain`, `examples/bench_app`, `examples/node_app`,
`examples/slcan_bridge`) builds only that project, with the same repo-local
ESP-IDF. Any further arguments are passed to `idf.py` instead of the default
build:

```sh
./build.sh node_updater menuconfig                    # general IDF settings; updater TWAI comes from cancfg
./build.sh node_updater -p /dev/ttyUSB0 flash monitor # first flash over USB/UART
powershell -ExecutionPolicy Bypass -File build.ps1 node_updater -p COM64 flash monitor
powershell -ExecutionPolicy Bypass -File build.ps1 brain -p COM63 flash
```

More examples: `./build.sh node_updater`, `./build.sh brain -p /dev/ttyACM0 flash`,
the bench-test app `./build.sh examples/bench_app`, the brain's USB-CAN bridge
`./build.sh examples/slcan_bridge`; on Windows `build.ps1 node_updater -p COM64 flash`,
`build.ps1 brain -p COM63 flash`.

`./build.sh node_updater -p /dev/ttyUSB0 flash` uses the project's default
4 MB table and **does not write `cancfg`**. It is a legacy/unprovisioned node
flash, not the IDU flash or a complete SOLO preset. For either SOLO board use
the [bundle and provisioning tool](first-flash.md#recommended-the-universal-bundle-and-tool).

idf.py arguments always follow a project. Without one (`build.ps1 -p COM5
flash`) the scripts stop with an error naming the projects, because the
command could be meant for either board. (It is positional rather than a
`-Project` switch, because PowerShell would bind idf.py's `-p` port option
to `-Project` by prefix.)

### Applications (OTA images)

A project whose `sdkconfig.defaults` uses the node updater's partition table
(`CONFIG_PARTITION_TABLE_CUSTOM_FILENAME` naming `node_updater/partitions.csv`)
is an application that reaches the node over CAN. Without further arguments
the scripts run `idf.py app` for it, since it only needs its OTA image, and
print that image's path:

```sh
./build.sh examples/bench_app                                # -> examples/bench_app/build/bench_app.bin
powershell -ExecutionPolicy Bypass -File build.ps1 examples/bench_app
powershell -ExecutionPolicy Bypass -File build.ps1 examples/bench_app menuconfig
```

`build`, `size` or `flash` on an application also builds its own bootloader,
which is never used. In a deep Windows checkout, that sub-project's object
paths exceed `MAX_PATH` and CMake warns, so stay with the default `app`.
Never `idf.py flash` an application onto a node; see
[writing-a-node-app.md](../guides/writing-a-node-app.md). The bench-test app
and its variants: [bench tests](../testing/bench/README.md) and
[build variants](../reference/configuration.md#build-variants).

### The SLCAN bridge is the exception

`examples/slcan_bridge` runs on the brain as a USB-CAN adapter
([slcan-bridge.md](../guides/slcan-bridge.md)). It is not an OTA image, so it
is flashed over USB with its own bootloader and partition table, and the
scripts' "OTA image to flash over CAN" message does not apply to it:

```sh
powershell -ExecutionPolicy Bypass -File build.ps1 examples/slcan_bridge -p COM8 flash
./build.sh examples/slcan_bridge -p /dev/ttyACM1 flash
```

Its bootloader sub-project triggers the same CMake path-length warning in a
deep Windows checkout; the build still completes.

### The brain

`brain/` (the brain updating itself over Ethernet, issue #5) is built and
first flashed the same way, with its own 16 MB partition table; after that
it takes new images over HTTP. It also stages a node image in its 2 MB
`node_fw` data partition and flashes a parked node over CAN using
`components/can_flash_host` (pure C) and `can_link` (TX6/RX7, 250 kbit/s).
The normal brain build includes these automatically; no extra build flags.
brain_ota has its own partition table, so the scripts run a full `build`
(also with no argument, after the node updater) and print its flash offsets.

### Already have ESP-IDF v5.3, or prefer Docker

```sh
cd node_updater && idf.py set-target esp32s3 build   # in an exported ESP-IDF v5.3 shell

docker run --rm -v "$PWD:/project" -w /project/node_updater espressif/idf:v5.3.6 \
    idf.py set-target esp32s3 build                   # from the repo root; brain alike
```

A `build/` configured in Docker (`/project`) is discarded automatically by
the scripts the next time they run natively; it is only build output.

## Outputs

The updater, in `node_updater/build/`:

| Offset (4 MB build) | File | What |
|---|---|---|
| `0x0` | `bootloader/bootloader.bin` | common recovery 2nd-stage bootloader (header patched by esptool for selected flash size) |
| `0x8000` | `partition_table/partition-table.bin` | default build's 4 MB node table |
| `0xE000` | `ota_data_initial.bin` | empty `otadata` → boots `factory` |
| `0x20000` | `can_updater.bin` | common CAN updater (factory offset differs on IDU) |

A successful updater build also creates `partition-table-node-4MB.bin` and
`partition-table-idu-16MB.bin`, then both:

- `esp32s3-can-updater-<version>.zip`: `bootloader.bin`, `can_updater.bin`,
  `ota_data_initial.bin`, both named partition tables, and `manifest.json`.
  Manifest format 1 identifies the `node-4MB` and `idu-16MB` layouts,
  flash sizes and flash offsets; it contains **no per-board `cancfg`**.
  [Provisioning](../guides/provisioning-can-ids.md) generates that image.
- `esp32s3-node-updater-<version>.zip`: the retained legacy 4 MB node bundle,
  with its original paths and flashing metadata.

Use the universal bundle for both SOLO boards. The first good release
carrying it is `c-cpp-esp32-v1.2.1` (v1.2.0's updater left the ROM port
unreachable after a 1200-baud touch; issue #24). A local `<version>`
comes from `git describe`; on a release tag it is the tag's version suffix.

The brain's firmware has the same set in `brain/build/`, with `brain_ota.bin`
at `0x20000` (`ota_0`) and its 16 MB `brain/partitions.csv` (bootloader `0x0`,
table `0x8000`, otadata `0xe000`, app `0x20000`). The brain's OTA image is
`brain/build/brain_ota.bin` (about 440 KB). An application's OTA image is
`build/<app>.bin`, e.g. `examples/bench_app/build/bench_app.bin`.

The offsets come from each project's `build/flasher_args.json`; the scripts
print them from there. Partition tables: [flash-layout.md](../concepts/flash-layout.md).

## CI and releases

`.github/workflows/esp32-build.yml` runs the [host tests](../testing/host-tests.md)
and `./build.sh` (node updater + brain), then builds both examples, on every
push / PR to `version/c-cpp/esp32`. It uploads three artifacts: legacy `esp32s3-node-updater`, universal
`esp32s3-can-updater`, and `esp32s3-brain` (the separate example). A
**`c-cpp-esp32-v*`** tag publishes versioned zips for all three, plus the
two app images. The first good universal-bundle release is
`c-cpp-esp32-v1.2.1` (v1.2.0 predates the 1200-baud touch fix, issue #24). Other targets
use their own `<lang>-<target>-v*` tags (`c-cpp-ra4m1-v*`,
`embassy-ra4m1-v*`).

```sh
git tag c-cpp-esp32-v1.0.0 && git push origin c-cpp-esp32-v1.0.0
```
