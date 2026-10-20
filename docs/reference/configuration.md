# Configuration

Kconfig options are set with `menuconfig` on a project
(`./build.sh <project> menuconfig`, `build.ps1 <project> menuconfig`). To keep
a board-specific configuration in git, put the overrides in that project's
`sdkconfig.defaults` (the generated `sdkconfig` is ignored), e.g.
`node_updater/sdkconfig.defaults`.

## Kconfig menus

### Factory updater: board profile, not Kconfig

The updater has **no TWAI pin/bitrate Kconfig menu**. The 24-byte record at
`cancfg + 0x50` supplies TWAI TX/RX, bitrate and GPIO hold-LOW mask per board.
The `BOARD_PROFILE_DEFAULT_*` constants in `src/boot/board_profile.h` are the
only fallback: TX 4, RX 5, 250 kbit/s, no holds for an absent/invalid record.
The updater logs `record` or `defaults` at startup. On TWAI init failure it
retries once with defaults unless a default pin is held LOW; then holds win
and the updater remains available over USB without CAN. The bootloader reads
the same record to assert holds before loading an app.

The record rejects GPIO0, GPIO19/20 and GPIO26..32 for either TWAI or holds,
and rejects holds on TWAI pins or above GPIO48. For board wiring and exact
byte format see [board profile](board-profile.md). Choose other pins based on
the actual PCB: the format's acceptance does not guarantee that a pin is free
of a board-specific function. The SOLO IDU uses TX6/RX7 and a GPIO3 hold;
the ODU uses TX4/RX5 with no holds.

### SLCAN bridge: `build.ps1 examples/slcan_bridge menuconfig` → **SLCAN Bridge**

| Option | Default | Notes |
|---|---|---|
| `SLCAN_BRIDGE_TWAI_TX_GPIO` | 6 | to the brain transceiver's TXD (TCAN332 `D`) |
| `SLCAN_BRIDGE_TWAI_RX_GPIO` | 7 | from its RXD (TCAN332 `R`) |
| `SLCAN_BRIDGE_BITRATE` | 250 kbit/s | used by an `O` without a prior `Sn`; python-can sends `Sn` |
| `NODE_RESET_GPIO` (*Node reset line*) | 3 | the node's EN (inverted); shared with `brain/` (`components/node_reset`) |

### Node app: `examples/node_app` → **Node app CAN**

| Option | Default | Notes |
|---|---|---|
| `NODE_APP_TWAI_TX_GPIO` | 4 | reference app only; match the *provisioned profile* and physical transceiver |
| `NODE_APP_TWAI_RX_GPIO` | 5 | reference app only; match the profile, not an updater Kconfig option |
| `NODE_APP_CAN_BITRATE_BPS` | 250000 | match the profile and the CAN host; `can_link` accepts 10k, 20k, 50k, 100k, 125k, 250k, 500k and 1M |

These example-app options remain build-time settings. The optional stock-core
[Arduino node-app helper](../guides/platformio-arduino.md#stock-arduino-node-app-helper)
loads provisioned IDs and holds; its caller configures the app's TWAI driver.

### Brain: `brain/` → **Brain OTA** and **Node reset line**

| Option | Default | Notes |
|---|---|---|
| `BRAIN_OTA_SKIP_MARK_VALID` | n | rollback test: stays `PENDING_VERIFY`, refuses uploads, rolls back on the next reset; layered by `sdkconfig.skip_mark_valid` |
| `NODE_RESET_GPIO` | 3 | the brain GPIO wired to the node's EN through an inverting stage |

The brain's CAN host pins and bitrate are fixed in `brain/main/node_flash.c`:
TX GPIO6, RX GPIO7, 250 kbit/s.

### Bench app: `examples/bench_app` → **Bench App**

| Option | Default | Notes |
|---|---|---|
| `BENCH_APP_SKIP_MARK_VALID` | n | skips `esp_ota_mark_app_valid_cancel_rollback()`; the next reset rolls back to the updater; layered by `sdkconfig.skip_mark_valid` |
| `BENCH_APP_CRASH_LOOP` | n | marks the image valid, then `abort()`s about 2 seconds after every boot; layered by `sdkconfig.crash_loop` |

The bench app's console is the **USB-Serial-JTAG** port (the primary console,
the only one that reads input). If you watch the node over UART0 instead,
switch it with `build.ps1 examples/bench_app menuconfig` →
*Component config → ESP System Settings → Channel for console output*.

## Committed `sdkconfig.defaults`

| Project | Notable settings |
|---|---|
| `node_updater` | build-time 4 MB `partitions.csv` (the universal bundle also generates `partitions_idu_16MB.csv`), rollback, no GPIO factory reset, the RTC contract below, `CONFIG_ESP_TASK_WDT_PANIC`; TinyUSB CDC-ACM, UART0-only console (`CONFIG_ESP_CONSOLE_SECONDARY_NONE`) |
| `brain` | `CONFIG_ESPTOOLPY_FLASHSIZE_16MB`, its own A/B `partitions.csv`, rollback, `CONFIG_ETH_SPI_ETHERNET_W5500`; octal PSRAM stays disabled; console on UART0 plus USB-Serial-JTAG secondary (`CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG`) |
| `examples/bench_app`, `examples/node_app` | the node's table via `../../node_updater/partitions.csv`, 4 MB, rollback, the RTC contract, console on USB-Serial-JTAG (`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG`) |
| `examples/slcan_bridge` | logs and panics to UART0 only: `CONFIG_ESP_CONSOLE_SECONDARY_NONE`, so the USB port carries SLCAN only |

**RTC contract.** The node updater, its bootloader and every node app must
reserve the same 16 custom RTC bytes, outside the IDF CRC
(`can_updater_client` refuses to build without it):

```ini
CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC=y
CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC_SIZE=0x10
# CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC_IN_CRC is not set
```

**Stale generated configs.** Existing generated sdkconfig files retain old
defaults. Save local settings, remove generated node/bench `sdkconfig` (and
`skip/sdkconfig` for that variant), then run that project's `fullclean` and
rebuild. Do not delete committed `sdkconfig.defaults`.

## Build variants

Test variants build in their own short build directory, layering an extra
defaults file, so they exist side by side with the normal `build/`. Keep the
directory name short: on Windows, a longer one pushes object paths past
`MAX_PATH`. All are git-ignored.

| Dir | Project | Layered / set | Output | Used by |
|---|---|---|---|---|
| `skip` | `examples/bench_app` | `sdkconfig.skip_mark_valid` | `examples/bench_app/skip/bench_app.bin` | [rollback test](../testing/bench/node-updater.md#d-rollback-a-bad-image-lands-back-in-the-updater) |
| `crash` | `examples/bench_app` | `sdkconfig.crash_loop` | `examples/bench_app/crash/bench_app.bin` | [crash loop](../testing/bench/node-recovery.md#confirmed-app-crash-loop) |
| `skip` | `brain` | `sdkconfig.skip_mark_valid` | `brain/skip/brain_ota.bin` | [brain rollback](../testing/bench/brain-ota.md#2-bad-skip_mark_valid-image-rollback-on-the-next-reset), [package rollback](../testing/bench/update-package.md#acceptance-and-fault-tests) |
| `p5b` | `brain` | `PROJECT_VER=p5-next` | `brain/p5b/brain_ota.bin` | [changed brain package](../testing/bench/update-package.md#acceptance-and-fault-tests) |
| `p6b` | `examples/node_app` | `PROJECT_VER=p6-next` | `examples/node_app/p6b/node_app.bin` | [CAN path package](../testing/bench/node-app.md#second-package-while-node_app-runs-can-path-no-gpio3-pulse) |

Pattern (one line; PowerShell: `powershell -ExecutionPolicy Bypass -File build.ps1` in place of `./build.sh`):

```sh
./build.sh examples/bench_app -B skip -D SDKCONFIG=skip/sdkconfig -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.skip_mark_valid" app
./build.sh examples/bench_app -B crash -D SDKCONFIG=crash/sdkconfig -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.crash_loop" app
./build.sh brain -B skip -D SDKCONFIG=skip/sdkconfig -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.skip_mark_valid" app
powershell -ExecutionPolicy Bypass -File build.ps1 brain -B p5b -D SDKCONFIG=p5b/sdkconfig -D PROJECT_VER=p5-next app
powershell -ExecutionPolicy Bypass -File build.ps1 examples/node_app -B p6b -D SDKCONFIG=p6b/sdkconfig -D PROJECT_VER=p6-next app
```
