# Using the shared code from PlatformIO (Arduino-ESP32)

The root `library.json` makes this branch a PlatformIO library,
`can-update-esp32`, for Arduino-ESP32 apps on the pioarduino platform: a node
app, or a brain replacement. History: #12.

Verified: throwaway brain-style and node-style projects (pioarduino
`espressif32` 55.03.311, Arduino-ESP32 3.3.11, `esp32-s3-devkitc-1`) build and
link. **Bench-validated on 2026-10-19:** the SOLO IDU (brain app on the stock
Arduino core) and SOLO ODU (stock-core node app with the flash-only
boot-counter clear) passed node flash, recover, scan, APP_INFO, brain OTA and
rollback, `/package`, and the power-cycle trip. The ODU's unconfirmed-image
rollback landed safely in the factory CAN updater, not the previous app.
See the [bench tests](../testing/bench/README.md) for the test procedure.

## Adding the library

```ini
lib_deps = https://github.com/XiaoKChen/Embedded-CAN-Bootloaders.git#c-cpp-esp32-v1.0.0
```

The SSH form (`git@github.com:XiaoKChen/Embedded-CAN-Bootloaders.git#...`)
works too. PlatformIO clones git dependencies with `--recursive`, so the
account installing it also needs read access to the repo's `.claude`
submodule. For a local checkout use `symlink://<path>`; `file://<path>` copies
the whole working tree, build outputs included.

Always compiled: `src/boot/{app_control,crc32,dfu_config,update_pkg,boot_counter,board_profile}.c`,
`components/can_flash_host/can_flash_host.c`, `brain/main/ota_update.c`. With
`custom_can_updater_client = yes` in the environment, the library's
`platformio/library_build.py` also compiles
`components/can_updater_client/{can_updater_client,recovery}.c` (node apps
only: `recovery.c` stops with `#error` without the RTC contract below).
With `custom_arduino_node_app = yes`, it compiles
`components/arduino_node_app/arduino_node_app.cpp` instead: an optional
stock-core helper for apps that own their TWAI controller. Do not enable
`custom_can_updater_client` for the stock-core path.

The app's own sources can then include, from C or C++: `app_control.h`,
`can_update.h`, `dfu_config.h`, `board_profile.h`, `crc32.h`, `update_pkg.h`,
`boot_counter.h`, `can_flash_host.h`, `can_updater_client.h`, `ota_update.h`;
C++ apps opting in can include `arduino_node_app.hpp`.

## Node apps

The same contract as
[integrating an existing application](writing-a-node-app.md#integrating-an-existing-application),
in `platformio.ini`:

```ini
[platformio]
; custom_sdkconfig recompiles the shared Arduino framework package in place:
; give this project its own package dir
packages_dir = ${platformio.core_dir}/packages-<project>

[env:node]
platform = espressif32
board = esp32-s3-devkitc-1
framework = arduino
board_upload.flash_size = 4MB
board_build.partitions = partitions.csv   ; a copy of node_updater/partitions.csv
lib_deps = https://github.com/XiaoKChen/Embedded-CAN-Bootloaders.git#c-cpp-esp32-v1.0.0
custom_can_updater_client = yes
custom_sdkconfig =
    CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC=y
    CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC_SIZE=0x10
    '# CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC_IN_CRC is not set'
```

- **Quote the `# ... is not set` line.** Unquoted, PlatformIO reads it as an
  ini comment and drops it; pioarduino strips the quotes.
- **Own `packages_dir`.** With `custom_sdkconfig`, pioarduino rebuilds the
  Arduino-ESP32 libraries from ESP-IDF inside the
  `framework-arduinoespressif32-libs` package and marks it with an
  `sdkconfig`. Any project sharing that package with a different (or no)
  `custom_sdkconfig` then reinstalls it, and the next node build recompiles
  it again. The first build downloads the packages and then
  compiles the ESP-IDF libraries (about 5 minutes on the test PC). On Windows
  keep that path short (as above): a deep `packages_dir` made the ESP-IDF
  compile fail with `ninja: fatal: CreateProcess: The parameter is incorrect
  (is the command line too long?)`, and run `pio` from PowerShell or cmd, not
  Git Bash/MSYS (ESP-IDF's `idf_tools.py` refuses MSYS).
- **Known build failure (pioarduino 55.03.311 with PlatformIO Core 6.2.0).**
  The `custom_sdkconfig` build stopped with
  `ModuleNotFoundError: No module named 'SCons.Tool.FortranCommon'`: the
  platform installs its own `tool-scons` (4.8.1) into `packages_dir` and
  deletes every `tool-scons@*` directory, including the SCons 4.11.1 the Core
  is running from. Workaround used for the verification build: copy the
  Core's `tool-scons` into `packages_dir` under a name not starting with
  `tool-scons` (e.g. `<packages_dir>/core-tool-scons`) before building.
- **Two limits that can rule `custom_sdkconfig` out** (found integrating the
  SOLO ODU, #14):
  - **No spaces in the project path.** ESP-IDF refuses a build directory
    containing whitespace, and `-fmacro-prefix-map` breaks on a project
    directory with one. A `subst` drive does not help: parts of the build
    resolve it back to the real path.
  - **No TinyUSB.** The rebuilt libraries lack the `arduino_tinyusb`
    component, so `-DARDUINO_USB_MODE=0` (USB-OTG CDC, custom USB
    descriptors) no longer compiles (`'USBSerial' was not declared`); only
    mode 1 (the chip's USB-Serial-JTAG) works.

  **Stock-core alternative.** Skip `custom_sdkconfig` and
  `custom_can_updater_client`; set `custom_arduino_node_app = yes` instead.
  The [helper below](#stock-arduino-node-app-helper) clears the flash boot
  counter after stable uptime, answers APP_INFO and ENTER_UPDATER, and applies
  provisioned LOW holds. It does not reserve the custom RTC bytes: the stock
  core may overwrite them, so RTC crash-loop counting and clean-restart marks
  are unavailable. Flash power-cycle recovery, a pending image's rollback and
  the factory updater still work. For an IDU, use the 16 MB IDU partition
  table, not the 4 MB node table.
- **Defer mark-valid.** Arduino's `initArduino()` marks a PENDING_VERIFY image
  valid before `setup()`, which defeats rollback into the updater. Opt out and
  confirm after the app's own self-test:

  ```cpp
  extern "C" bool verifyRollbackLater() { return true; }
  // ... once healthy:
  esp_ota_mark_app_valid_cancel_rollback();
  ```

- **CPU0.** `setup()` and `loop()` run on core 1. Call
  `can_updater_client_start_health()`, and make every deliberate
  `esp_restart()` / `can_updater_client_enter_updater()`, from a task pinned
  to CPU0 ([recovery contract](writing-a-node-app.md#recovery-contract)):

  ```cpp
  static void system_task(void *)
  {
      if (can_updater_client_start_health() != ESP_OK) { /* report it */ }
      // self-test, mark valid, then serve restart / enter-updater requests here
  }

  void setup()
  {
      xTaskCreatePinnedToCore(system_task, "system", 4096, nullptr, 5, nullptr, 0);
  }
  ```

- **Do not use PlatformIO's default node upload.** It writes the Arduino
  bootloader at `0x0` (replacing the node's recovery bootloader), the partition
  table, Arduino's `boot_app0.bin` over `otadata` at `0xe000`, and the app at
  `ota_0` (`0x80000`). Deliver `.pio/build/<env>/firmware.bin` over CAN
  ([updating-nodes.md](updating-nodes.md)), or write only `ota_0` at `0x80000`
  plus a matching `otadata` with esptool. The SOLO ODU overrides its upload
  recipe to write only `otadata` and `ota_0`; its `pio run -t upload` was
  verified not to touch `0x0`, `0x8000`, or `0x20000`.
- **Stock Arduino app USB reset tip (not the factory updater).** After an app USB flash the board may stay in the
  flasher stub even when esptool says `Hard resetting via RTS pin`. Boot the
  app with `esptool --chip esp32s3 -p <ROM port> --before no_reset --after watchdog-reset chip-id`
  (esptool 5.x).
- **APP_INFO strings and replies.** The app descriptor does not carry the
  app's own identity: a stock Arduino build names the Arduino lib builder
  (`project_name` `arduino-lib-builder`, `version` its git hash), and the
  `custom_sdkconfig` test build named the project directory with version `1`.
  An app answering APP_INFO passes its own name and version to
  `app_control_encode_info()` instead of `esp_app_get_description()`. Send
  APP_INFO and ENTER_UPDATER replies from a task that can wait for TWAI TX
  queue space (the SOLO ODU uses a 50 ms per-frame wait). A zero-wait send
  inside the CAN receive path lost reply frames immediately after the app's
  own publish bursts (SOLO ODU #30/#31).

## Stock Arduino node-app helper

For an Arduino-ESP32 app that already installs/receives TWAI frames, set
`custom_arduino_node_app = yes` in **that PlatformIO environment**. The library
then compiles `components/arduino_node_app/` (C++); without the flag, it does
not. Copy the matching [partition table](../concepts/flash-layout.md), set
`board_upload.flash_size = 4MB` for the ODU or `16MB` for the IDU, and do
not run PlatformIO's default full-chip upload. The current `library.json`
export includes the 4 MB CSV but not `partitions_idu_16MB.csv`: copy the IDU
table from a full repository checkout, not from the installed library
package. The stock
core does not need `custom_sdkconfig` or `custom_can_updater_client` for this
helper.

```ini
[env:odu]
framework = arduino
board_upload.flash_size = 4MB
board_build.partitions = partitions.csv  ; copy of node_updater/partitions.csv
custom_arduino_node_app = yes
```

The app supplies Arduino's rollback hook **before** `setup()` and wires the
helper into its existing task-level TWAI receive path:

```cpp
#include <arduino_node_app.hpp>
#include <driver/twai.h>

extern "C" bool verifyRollbackLater() { return true; }

void setup() {
    // Install/start the app's own TWAI driver on the board's physical pins.
    if (arduino_node_app::begin() != ESP_OK) { /* report startup failure */ }
}

void on_twai_message(const twai_message_t &frame) { // call from a task, not an ISR
    if (!frame.extd && !frame.rtr)
        arduino_node_app::on_standard_frame(frame.identifier,
                                            frame.data_length_code, frame.data);
}

void on_self_test(bool healthy) {
    arduino_node_app::confirm_when_healthy(healthy);
}
```

Call `begin(Config)` **once early**, before registering RX callbacks. Keep all
profile hold pins LOW. The helper loads v2 `cancfg` IDs (`command_id()` and
`reply_id()` expose the pair) and validated profile holds, but **does not
install TWAI, choose app pins/bitrate, filter frames or receive frames**; the
app must match its driver to the board's transceiver/profile. Pass only
standard DATA frames to `on_standard_frame(id, dlc, data)` from one task. It
parses requests there and answers APP_INFO/ENTER_UPDATER from a core-0 task
with a bounded TWAI TX wait. APP_INFO takes label/version from `USB_PRODUCT`
split at the last space, not the ESP app descriptor. ENTER_UPDATER drains its
ACK before selecting factory and restarting. After `stable_uptime_ms`, it
clears the untripped flash boot counter, not a latched recovery trip.

`Config` fields are `task_core` (default 0), `task_priority` (default 1),
`stable_uptime_ms` (default `BOOT_COUNTER_STABLE_MS`),
`reply_tx_wait_ms` (default 50), `ack_drain_ms` (default 20), and
`owns_image_confirmation` (default true). `confirm_when_healthy(true)` marks
a pending image valid once after the app's own health check; set
`owns_image_confirmation = false` when an existing OTA owner does that. The
app must define `verifyRollbackLater()` either way to prevent Arduino from
confirming an untested image during `initArduino()`. Details:
`components/arduino_node_app/include/arduino_node_app.hpp`. This is an
opt-in replacement for a stock app's hand-written CAN entry/flash-counter
path, **not** the full custom-RTC recovery client. The universal updater's
USB identity is independent of the running app's `USB_PRODUCT`.

## Brain apps

- Copy `brain/partitions.csv` (`board_build.partitions`) and set
  `board_upload.flash_size = 16MB`.
- No `custom_sdkconfig` and no `custom_can_updater_client`: the stock
  Arduino-ESP32 libraries already build with
  `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`. The `verifyRollbackLater()` note
  above applies if the app should self-test before confirming;
  `ota_update_begin()` refuses while the running image is PENDING_VERIFY.
- Self-OTA: `ota_update.h` (begin / write / finish into the inactive slot).
- Flashing nodes: `can_flash_host.h` with the app's own CAN transport behind
  the `send` / `receive` / `clock_ms` / `read` callbacks of
  `can_flash_host_t`.
