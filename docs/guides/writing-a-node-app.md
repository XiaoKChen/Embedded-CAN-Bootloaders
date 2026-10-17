# Writing a node app

Start from **`examples/node_app`**, the reference for real node firmware.
`examples/bench_app/` is a minimal working example without CAN traffic of its
own (bench tests only).

## The reference app

`examples/node_app` is an OTA app on the node's 4 MB table that calls
`can_updater_client_start_health()`, loads the node's update IDs from
`cancfg` (else `0x79E`/`0x79F`), starts TWAI at 250 kbit/s on GPIO4/GPIO5,
self-tests, marks itself valid, and runs a CAN task (`main/can_service.c`)
answering APP_INFO and ENTER_UPDATER on those IDs
([can-protocol.md](../reference/can-protocol.md#app-control-app_info-and-enter_updater)).
With it, the brain's package relay and `/node/flash`, and
`can_flash.py --enter-updater`, park a running node over CAN without the
GPIO3 reset line. History: #11.

Real firmware starts by replacing `app_run()` in `main/node_app.c` (marked
**YOUR APPLICATION HERE**) and extending `self_test_passed()`.

```sh
powershell -ExecutionPolicy Bypass -File build.ps1 examples/node_app
./build.sh examples/node_app          # Linux / macOS
```

The OTA image is `examples/node_app/build/node_app.bin`. Its TWAI pins and
bitrate (**Node app CAN** menu) must match the updater
([configuration.md](../reference/configuration.md#node-app-examplesnode_app--node-app-can)).

## Integrating an existing application

An application that should be updatable over CAN (an Arduino-ESP32 / PlatformIO
app: [platformio-arduino.md](platformio-arduino.md)):

1. **Use the updater's layout.** Copy `node_updater/partitions.csv` (or point
   at it, as `examples/bench_app` does) and set, in the app's
   `sdkconfig.defaults`:

   ```
   CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
   CONFIG_PARTITION_TABLE_CUSTOM=y
   CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
   CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
   ```

   plus the [recovery contract](#recovery-contract) below.

2. **Add the client component.** Point the app at it (or copy the folder):

   ```cmake
   # app CMakeLists.txt, before include(project.cmake)
   set(EXTRA_COMPONENT_DIRS "<this repo>/components/can_updater_client")
   ```

   and `REQUIRES can_updater_client` in the component that handles CAN.

3. **Enter the updater on request.** Either answer ENTER_UPDATER natively
   (copy `examples/node_app/main/can_service.c`), or, where the app handles
   its own "enter bootloader" request (on the Airform boards: the
   `/enter_bootloader` route that `can_flash.py --enter-bootloader` sends over
   ISO-TP):

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

5. **Build and flash it over CAN** — `build/<app>.bin` is the OTA image
   ([updating-nodes.md](updating-nodes.md)).

Do **not** `idf.py flash` an application onto an updater board: it would
rewrite the bootloader with the app's configuration and put the app into the
first app partition, which is `factory` — overwriting the updater. Deliver
applications over CAN (or with esptool to `ota_0` at `0x80000` plus a matching
`otadata`).

## Recovery contract

Call `can_updater_client_start_health()` once early in `app_main`, check its
result, and keep the main task/deliberate restart caller on CPU0. The updater,
`examples/node_app` and the bench variants already do this. All app builds
must reserve the same 16 custom RTC bytes as `node_updater/sdkconfig.defaults`:

```ini
CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC=y
CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC_SIZE=0x10
# CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC_IN_CRC is not set
```

Older app binaries retain flash-layout compatibility but lack the
clean-restart mark and stable clear and may use the reserved RTC area;
rebuild them for full recovery support. Flash-bit recovery itself does not
require their cooperation. Why: [recovery.md](../concepts/recovery.md).
