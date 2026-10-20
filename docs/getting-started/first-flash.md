# First flash over USB (once per board)

Build the updater bundle first: [build.md](build.md). Provision each SOLO board through its **own** USB port with [ESP-PROG-2-PY-CLI](https://github.com/XiaoKChen/ESP-PROG-2-PY-CLI). The same `bootloader.bin` and `can_updater.bin` serve ODU and IDU; the tool selects the table, flash size and [board profile](../reference/board-profile.md) for each. CAN app updates cannot replace the bootloader or partition table. Wiring: [wiring.md](wiring.md).

## Order: IDU (brain) first

The IDU's GPIO3 controls the ODU's EN through an inverting stage (HIGH holds the ODU in reset). A blank IDU, or one in ROM download, may leave GPIO3 floating and prevent the ODU from enumerating. The IDU profile drives GPIO3 LOW after 2nd-stage bootloader init, before partition-table logging. It **cannot** hold it during ROM boot. Flash the IDU first, let it boot the updater or app, then connect or reset the ODU. Details: [GPIO3](wiring.md#brain-gpio3--node-en-reset-line).

## Identifying the ports

`COM8` (IDU) and `COM7` (ODU) are examples; use your own ports. USB VID:PID is `303A:1001` for the updater's TinyUSB CDC-ACM device. An IDU or ODU with no app flashed boots the updater and enumerates with product **SOLO IDU Controller** or **SOLO ODU Controller** and manufacturer **Normal Corporation**. Its serial is the chip MAC as 12 uppercase hex digits. ROM download mode instead shows **Espressif's** USB identity; it does not use the provisioned name. Unplug one board to distinguish ports if necessary. UART0 is the updater's console; the updater does **not** mirror logs onto its USB CDC port. Apps and the SLCAN bridge can use their own USB console.

## Recommended: the universal bundle and tool

From the ESP-PROG-2-PY-CLI checkout, unzip a local `esp32s3-can-updater-<version>.zip` built from this branch ([outputs](build.md#outputs)), then use:

```sh
uv run esp-prog2-flasher --flash-solo idu --port COM8 --bundle path/to/unzipped-bundle --app idu.bin
uv run esp-prog2-flasher --flash-solo odu --port COM7 --bundle path/to/unzipped-bundle --app odu.bin
```

`--app` is optional. Without it blank `otadata` boots the updater (`factory`), so USB shows the board's provisioned product name. An IDU app must support both its brain role and APP_INFO/ENTER_UPDATER on `0x07/0x08`; ODU uses `0x05/0x06`. Omit the board key on an interactive terminal to choose a preset. `--migrate-stock` erases the old `0x10000..0x1FFFF` region before flashing a stock-layout board. `--manufacturer NAME` changes the v2 manufacturer. The first good release of this universal bundle is `c-cpp-esp32-v1.2.1` (published; v1.2.0's updater cannot reach ROM download from a 1200-baud touch); `--bundle-tag c-cpp-esp32-v1.2.1` fetches it with authenticated `gh` instead of `--bundle`. [Provisioning](../guides/provisioning-can-ids.md) gives the full sequence and a manual esptool recipe.

The tool uses one `write-flash --flash-mode dio --flash-freq 80m --flash-size 16MB|4MB` for bootloader `0x0`, selected table `0x8000`, erased `otadata` `0xE000`, updater at the layout's factory offset and v2+profile `cancfg` `0x11000`. It verifies `cancfg` by reading it back. With `--app`, it also writes `ota_0` and an OTA selection image. Its final esptool operation uses `--after watchdog-reset`: RTS hard reset over the USB connection did not reboot these boards on the bench. The flash-size setting patches the **same** bootloader image's header at write time.

## Download mode and the BOOT button

On a board already running the updater, selecting **1200 baud** on its CDC port immediately restarts into ROM download (from v1.2.1 the updater first hands USB back to USB-Serial-JTAG, so the ROM port enumerates); esptool can then connect without holding BOOT. For a blank board or if USB CDC is unavailable, hold **BOOT** (GPIO0) during reset/plug-in, release it, and flash. If a reset leaves the chip at `waiting for download`, use `--after watchdog-reset` or power-cycle **without** BOOT. ROM download identifies itself as Espressif, not the provisioned board name. UART0 (GPIO43 TX, GPIO44 RX) carries the updater and early boot logs.

## The brain: `brain_ota`

The separate `brain/` ESP-IDF example still builds and flashes its **own** `brain/partitions.csv` (no `factory`); it is not the IDU universal-bundle layout. For that example, connect Ethernet to the PC's DHCP network and flash over USB:

```sh
powershell -ExecutionPolicy Bypass -File build.ps1 brain -p COM8 flash monitor
./build.sh brain -p /dev/ttyACM0 flash monitor
```

This writes its bootloader, 16 MB table, blank `otadata` and `brain_ota.bin` to `ota_0`. `brain_ota` drives GPIO3 LOW in `app_main`, gets an IP and serves `POST /ota`, `/node/*` and `/package`. Note the `IP acquired` address; `<ip>` in other guides means that address. A normal `/ota` upload never installs a new partition table. The example's USB flash is not a substitute for `--flash-solo idu`, and its app should not be assumed to satisfy the IDU node role on `0x07/0x08`.

The SLCAN bridge is alternative brain firmware, not a simultaneous service: [slcan-bridge.md](../guides/slcan-bridge.md). Flashing it with its own bootloader/table also replaces the universal-bundle layout.

## The node: the updater

For the 4 MB reference node bench, use `--flash-solo odu` without `--app` to boot into the factory updater with provisioned `0x05/0x06` IDs. The older `esp32s3-node-updater-<version>.zip` remains available for legacy 4 MB installs; an **unprovisioned** v2 block there uses `0x79E/0x79F`, and an absent profile selects TX 4/RX 5, 250 kbit/s, no holds. To run the existing bench scripts against that default pair, use the legacy flash path or pass the ODU pair explicitly. The 16 MB IDU instead uses its own layout, TX 6/RX 7 and GPIO3 hold. Do not flash the 4 MB node table onto the IDU as a universal setup.

For a previously installed node, update the bootloader/table/updater once over USB without a full-chip erase if you need to retain OTA slots and `cancfg` ([recovery install](../testing/bench/node-recovery.md#install-once-over-usb)). Plan a migration before changing its partition layout; `--flash-solo` installs blank `otadata`.

## The monitor

Use UART0 for factory-updater logs. The ESP-IDF example apps may use USB for their own monitor and input (the bench app's `u` command). Reopening `monitor` can reset a board; `--no-reset` avoids that. Opening the brain's port with pyserial default DTR/RTS can also reset it. For the Ethernet example, `curl.exe http://<ip>/` checks status without reopening a serial port.

Next: [updating nodes](../guides/updating-nodes.md) over CAN.
