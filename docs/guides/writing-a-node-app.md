# Writing a node app

Start from **`examples/node_app`**, the reference for real node firmware.
`examples/bench_app/` is a minimal working example without CAN traffic of its
own (bench tests only).

## The reference app

`examples/node_app` is an OTA app for the **4 MB reference node**. It calls
`can_updater_client_start_health()`, loads the v2 CAN IDs from `cancfg`
(default `0x79E/0x79F`), and starts TWAI on its own example Kconfig defaults
GPIO4/GPIO5 at 250 kbit/s. Those app settings must match the board's
[provisioned profile](../reference/board-profile.md), which independently
configures the factory updater. The reference app self-tests, marks itself
valid, and answers APP_INFO/ENTER_UPDATER on its own pair
([CAN protocol](../reference/can-protocol.md#app-control-app_info-and-enter_updater)).
The SOLO IDU instead uses the `idu-16MB` layout and its `0x07/0x08` pair:
it is both brain and node, and its app must answer on that pair.
With it, the brain's package relay and `/node/flash`, and
`can_flash.py --enter-updater`, park a running node over CAN without the
GPIO3 reset line. History: #11.

Real firmware starts by replacing `app_run()` in `main/node_app.c` (marked
**YOUR APPLICATION HERE**) and extending `self_test_passed()`.

```sh
powershell -ExecutionPolicy Bypass -File build.ps1 examples/node_app
./build.sh examples/node_app          # Linux / macOS
```

The OTA image is `examples/node_app/build/node_app.bin`. The example's TWAI pins and bitrate (**Node app CAN** menu) must match its
physical board/profile; they are not updater Kconfig values
([configuration.md](../reference/configuration.md#node-app-examplesnode_app--node-app-can)).
For a stock-core Arduino-ESP32 app that already owns TWAI, prefer the
opt-in [Arduino node-app helper](platformio-arduino.md#stock-arduino-node-app-helper)
(`custom_arduino_node_app = yes`) to hand-writing APP_INFO, ENTER_UPDATER,
profile holds and the stable flash-counter clear.

## Integrating an existing application

An application that should be updatable over CAN (an Arduino-ESP32 / PlatformIO
app: [platformio-arduino.md](platformio-arduino.md)):

1. **Use the board's layout.** For a 4 MB node copy
   `node_updater/partitions.csv` (as `examples/bench_app` does), and set:

   ```
   CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
   CONFIG_PARTITION_TABLE_CUSTOM=y
   CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
   CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
   ```

   Put these in the ESP-IDF app's `sdkconfig.defaults`, plus the
   [recovery contract](#recovery-contract). On the 16 MB IDU use
   `node_updater/partitions_idu_16MB.csv`, 16 MB flash size and `ota_0`
   at `0x20000` instead.

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

Do **not** `idf.py flash` an application onto a provisioned updater board:
it writes the app project's bootloader and table, which may replace the
recovery bootloader and the selected board layout. On a 4 MB node a full
flash can also put the app in the first app partition, `factory`, overwriting
the updater. Deliver apps over CAN, or use the provisioning tool's `--app`
option with the matching layout (4 MB `ota_0` at `0x80000`; IDU `ota_0`
at `0x20000`) and matching `otadata`.

## Recovery contract

For ESP-IDF apps using `can_updater_client`, call
`can_updater_client_start_health()` once early in `app_main`, check its
result, and keep the main task/deliberate restart caller on CPU0. The updater,
`examples/node_app` and the bench variants already do this. These builds
must reserve the same 16 custom RTC bytes as `node_updater/sdkconfig.defaults`:

```ini
CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC=y
CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC_SIZE=0x10
# CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC_IN_CRC is not set
```

Older app binaries retain flash-layout compatibility but lack the
clean-restart mark and stable clear and may use the reserved RTC area;
rebuild them for full recovery support. Flash-bit recovery itself does not
require their cooperation. A stock Arduino app using the
[helper](platformio-arduino.md#stock-arduino-node-app-helper) does not reserve
these RTC bytes. It retains flash-counter clearing and pending-image rollback,
but not the custom RTC crash-loop or clean-restart contract. Why:
[recovery.md](../concepts/recovery.md).
