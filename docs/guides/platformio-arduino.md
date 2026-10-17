# Using the shared code from PlatformIO (Arduino-ESP32)

The root `library.json` makes this branch a PlatformIO library,
`can-update-esp32`, for Arduino-ESP32 apps on the pioarduino platform: a node
app, or a brain replacement. History: #12.

Verified: throwaway brain-style and node-style projects (pioarduino
`espressif32` 55.03.311, Arduino-ESP32 3.3.11, `esp32-s3-devkitc-1`) build and
link. **Not bench-tested:** nothing in this guide has run on a board yet; run
the [bench tests](../testing/bench/README.md) before deploying such an app.

## Adding the library

```ini
lib_deps = https://github.com/XiaoKChen/Embedded-CAN-Bootloaders.git#c-cpp-esp32-v1.0.0
```

The SSH form (`git@github.com:XiaoKChen/Embedded-CAN-Bootloaders.git#...`)
works too. PlatformIO clones git dependencies with `--recursive`, so the
account installing it also needs read access to the repo's `.claude`
submodule. For a local checkout use `symlink://<path>`; `file://<path>` copies
the whole working tree, build outputs included.

Always compiled: `src/boot/{app_control,crc32,dfu_config,update_pkg,boot_counter}.c`,
`components/can_flash_host/can_flash_host.c`, `brain/main/ota_update.c`. With
`custom_can_updater_client = yes` in the environment, the library's
`platformio/library_build.py` also compiles
`components/can_updater_client/{can_updater_client,recovery}.c` (node apps
only: `recovery.c` stops with `#error` without the RTC contract below).

The app's own sources can then include, from C or C++: `app_control.h`,
`can_update.h`, `dfu_config.h`, `crc32.h`, `update_pkg.h`, `boot_counter.h`,
`can_flash_host.h`, `can_updater_client.h`, `ota_update.h`.

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

- **Never `pio run -t upload` a node app.** PlatformIO's default upload
  writes the Arduino bootloader at `0x0` (replacing the node's recovery
  bootloader), the partition table, Arduino's `boot_app0.bin` over `otadata`
  at `0xe000`, and the app at `ota_0` (`0x80000`). Deliver
  `.pio/build/<env>/firmware.bin` over CAN
  ([updating-nodes.md](updating-nodes.md)), or write only `ota_0` at
  `0x80000` plus a matching `otadata` with esptool.
- **APP_INFO strings.** The app descriptor does not carry the app's own
  identity: a stock Arduino build names the Arduino lib builder
  (`project_name` `arduino-lib-builder`, `version` its git hash), and the
  `custom_sdkconfig` test build named the project directory with version `1`.
  An app answering APP_INFO passes its own name and version to
  `app_control_encode_info()` instead of `esp_app_get_description()`.

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
