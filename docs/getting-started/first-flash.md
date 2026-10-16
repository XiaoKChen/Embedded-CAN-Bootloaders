# First flash over USB (once per board)

The node's updater, bootloader and partition table, and the brain's
firmware, go on over the S3's USB-Serial-JTAG (or UART0) with esptool. After
that, node applications arrive over CAN and brain images over Ethernet.
Build first: [build.md](build.md). Wiring: [wiring.md](wiring.md).

## Order: brain first

Flash the brain before the node. The brain's GPIO3 drives the node's EN
through an inverting stage (HIGH holds the node in reset). While the brain
is blank, in download mode, being flashed or still booting, GPIO3 floats and
can hold the node in reset: the node then does not appear as a USB device at
all. Both brain firmwares (`brain_ota`, the SLCAN bridge) drive GPIO3 LOW from
boot. Once the brain runs, re-plug the node (holding BOOT for its first flash)
and it enumerates. Details: [wiring.md, GPIO3](wiring.md#brain-gpio3--node-en-reset-line).

## Identifying the ports

The commands use **`COM63`** or **`COM8`** for the brain and **`COM7`** for
the node; yours will differ (`COM7`, `COM12`, …; `/dev/ttyACM0`,
`/dev/ttyACM1` on Linux). Both boards enumerate as the same USB device
(VID:PID `303A:1001`), so tell them apart by unplugging one, or list them with
their serial numbers (the chip's MAC):

```sh
uv run --with pyserial python -m serial.tools.list_ports -v
```

## Download mode and the BOOT button

esptool resets the S3 into download mode over USB-Serial-JTAG by itself. If
it cannot connect (observed when the board runs firmware that does not answer
esptool's reset), hold **BOOT** (GPIO0) while plugging in the board's USB (or
while pressing reset), release it, and flash again. Observed on these boards:
after a flash from a BOOT-button download mode, esptool's "Hard resetting via
RTS pin" can leave the chip in download mode. Unplug and re-plug the USB (or
power-cycle) **without** BOOT to start the new firmware. If a UART0 log shows
`waiting for download`, the chip sits in its ROM download mode; power-cycle it.

## The brain: `brain_ota`

Connect the brain's Ethernet to the same DHCP network as the PC, and its USB
port to the PC, then:

```sh
powershell -ExecutionPolicy Bypass -File build.ps1 brain -p COM63 flash monitor
./build.sh brain -p /dev/ttyACM0 flash monitor
```

`flash` writes the bootloader, the 16 MB table, a blank `otadata` and the app
into `ota_0`, replacing whatever the brain held (the bridge included); add
`erase-flash` before `flash` for a clean slate. `build.ps1 brain -p COM63 monitor`
reopens the monitor later.

Expected on the console. The version is the repo's `git describe`; the
2nd-stage bootloader's `boot:` lines may appear on UART0 only.

```
I (...) brain_ota: GPIO3 (node reset) driven LOW
I (...) brain_ota: brain_ota version v1.0.2-... (ESP-IDF v5.3.6), running from ota_0 (0x20000), OTA state VALID
I (...) eth_w5500: W5500 on SPI2 at 20 MHz (MOSI 11, MISO 13, SCLK 12, CS 10, INT 9, RST 14), MAC xx:xx:xx:xx:xx:xx
I (...) eth_w5500: Ethernet started; waiting for link and DHCP
I (...) http_ota: HTTP port 80: GET /, POST /ota, GET /node/scan, POST /node/flash, POST /node/recover, GET /node/app_info, GET /node/status, POST /package, GET /package/status
I (...) eth_w5500: link up
I (...) eth_w5500: ==================================================
I (...) eth_w5500: IP acquired: 192.168.1.57   ->  http://192.168.1.57/
I (...) eth_w5500: ==================================================
```

On the first boot after a USB flash `otadata` is blank; the bootloader
records `ota_0` as VALID, so no self-test runs. **Note the IP** from the
`IP acquired` line (or find the MAC from the `W5500` line in your router's
DHCP table). Elsewhere in these docs, `<ip>` stands for it.

`W5500 driver install failed`, or no `link up`, means a wiring, reset or
cable problem. Nothing over HTTP works until `IP acquired` shows.

To use the brain as a USB-CAN adapter instead, flash the SLCAN bridge:
[slcan-bridge.md](../guides/slcan-bridge.md). **While brain_ota runs, the
brain is not the SLCAN bridge**, and vice versa: re-flash over USB to switch.

## The node: the updater

This writes the bootloader, partition table, `otadata` and the updater
(`factory`). `erase-flash` first clears whatever the board held before, so
`ota_0`/`ota_1` start empty and the `cancfg` block is unprovisioned (default
IDs `0x79E`/`0x79F`).

```sh
powershell -ExecutionPolicy Bypass -File build.ps1 node_updater -p COM7 erase-flash flash monitor
./build.sh node_updater -p /dev/ttyACM0 erase-flash flash monitor
```

or with esptool directly, from `node_updater/build/` (esptool 4.x, the
version ESP-IDF v5.3 installs):

```sh
python -m esptool --chip esp32s3 -p COM7 erase_flash
python -m esptool --chip esp32s3 -p COM7 -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 4MB --flash_freq 80m 0x0 bootloader/bootloader.bin 0x8000 partition_table/partition-table.bin 0xe000 ota_data_initial.bin 0x20000 can_updater.bin
```

Without erasing, the short forms are:

```sh
./build.sh node_updater -p /dev/ttyUSB0 flash
# or, from node_updater/build/ in an environment with esptool:
python -m esptool --chip esp32s3 write_flash @flash_args
```

**Expected on the console** (the updater logs to UART0 and, as secondary
output, to USB-Serial-JTAG):

```
I (...) boot: Defaulting to factory image
I (...) boot: Loaded app from partition at offset 0x20000
...
I (...) can_updater: parked: cmd ID 0x79E, reply ID 0x79F, 250000 bit/s, slot ota_0 (1835008 bytes)
```

The same updater image serves both boards: ESP32-S3-WROOM-1U-N16R8 (16 MB
flash; the layout uses the first 4 MB) and ESP32-S3FH4R2 (4 MB). The updater
leaves PSRAM unconfigured, so octal vs. quad PSRAM does not matter to it.

**Existing nodes** flashed before the recovery bootloader need bootloader +
partition table + updater flashed over USB once (without `erase-flash` to
keep app slots and `cancfg`); CAN cannot replace the bootloader. See
[bench/node-recovery.md, install once](../testing/bench/node-recovery.md#install-once-over-usb).

## The monitor

`monitor` reconnects across resets (Ctrl+] quits; Ctrl+T then Ctrl+R resets
the board). Reopening it resets the board, unless you add `--no-reset`
(`build.ps1 node_updater -p COM7 monitor --no-reset`). To see an app's logs
with its symbols, run the monitor from its project:
`build.ps1 examples/bench_app -p COM7 monitor`.

**Opening the brain's port can reset the brain.** Observed on hardware: a
tool that (re)opens the port with pyserial's default DTR/RTS (a plain script,
or `monitor` without `--no-reset`) resets the S3. Keep one `monitor` session
open, or watch the brain with `curl.exe http://<ip>/` instead.

Next: [updating nodes](../guides/updating-nodes.md) over CAN.
