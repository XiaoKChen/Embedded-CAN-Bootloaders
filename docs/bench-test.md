# Bench Test — ESP32-S3 Node + CAN Updater (Phase 0, issues #2 and #3)

Tests the **node**'s CAN updater from a PC with `tools/can-flash`, using the
**brain** board as the USB-CAN adapter: the brain runs the SLCAN bridge
(`examples/slcan_bridge`), so python-can talks to it as an ordinary `slcan`
adapter. The node is the custom **ESP32-S3FH4R2** board (4 MB in-package
flash) with a **TCAN332DR** on TWAI **TX = GPIO4, RX = GPIO5**; the brain is
the **ESP32-S3-WROOM-1U-N16R8** board with a TCAN332DR on **TX = GPIO6,
RX = GPIO7**. The bus runs at **250 kbit/s**. A commercial USB-CAN adapter
works in the brain's place (section 7). Section 8 tests something else: the
brain updating itself over Ethernet (issue #5). Section 9 tests the brain
flashing a parked node over CAN with its C host (Phase 2, issue #8).

You need:

- the node, with a USB cable to its USB-Serial-JTAG port (GPIO19/20). The
  node keeps this connection through the tests: it carries the first
  updater flash and the node's console (logs, and the bench app's `u`
  command);
- the brain, with a USB cable to its native USB port (GPIO19/20). It is
  flashed with the bridge once; after that this port is the SLCAN adapter;
- three wires between the boards: CANH, CANL and GND;
- `uv` (or Python 3.9+ with `tools/can-flash/requirements.txt` plus
  `pyserial`);
- optional: a 3.3 V USB-UART on the brain's UART0 (GPIO43 TX, GPIO44 RX,
  115200 baud) to read the bridge's log (bus errors, frame loss). The tests
  do not need it.

**Ports.** The commands use **`COM8` for the brain** (`--interface slcan
--channel COM8`) and **`COM7` for the node**; yours will differ (`COM7`,
`COM12`, …; `/dev/ttyACM0`, `/dev/ttyACM1` on Linux). Both boards enumerate
as the same USB device (VID:PID `303A:1001`), so tell them apart by unplugging
one, or list them with their serial numbers (the chip's MAC):

```sh
uv run --with pyserial python -m serial.tools.list_ports -v
```

**Bitrate.** `--bitrate 250000` (can-flash's default) makes python-can send
the SLCAN command `S5` before opening the channel, and the bridge opens TWAI
at 250 kbit/s, matching the node.

## 1. Wiring

Each board carries its own TCAN332DR (3.3 V, so its R output feeds the S3
directly). Between the boards run only the bus and ground:

```
brain: ESP32-S3-WROOM-1U-N16R8                     node: ESP32-S3FH4R2
GPIO6 (TWAI TX) ──► D  TCAN332DR       TCAN332DR  D ◄── GPIO4 (TWAI TX)
GPIO7 (TWAI RX) ◄── R                             R ──► GPIO5 (TWAI RX)
                       CANH ──────────────── CANH
                       CANL ──────────────── CANL
GND ────────────────── GND ───────────────── GND ────── GND
                       120 Ω CANH–CANL       120 Ω CANH–CANL
GPIO3 ┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄ EN  (optional, see below)
```

- **CANH to CANH, CANL to CANL.**
- **Termination: 120 Ω at each end** of the bus, one at the brain and one at
  the node. This repo does not record whether either board already fits a
  terminator (120 Ω, or a split 2 × 60 Ω, across CANH–CANL next to the
  transceiver): **check both schematics.** Then, with both boards unpowered
  and wired together, measure CANH–CANL: about **60 Ω** is right; about
  **120 Ω** means one end has no terminator; about **40 Ω** means three
  (remove one); open means none.
- **Common ground:** a GND wire between the boards, next to CANH/CANL.
  Without it the bus works intermittently or not at all.
- **GPIO3 → node reset** (the brain's reset line to the node) goes through an
  inverting stage on this board: **GPIO3 HIGH holds the node in reset, LOW
  lets it run.** The bridge firmware drives GPIO3 LOW from boot, so flash the
  bridge onto the brain first. While the brain is blank, in download mode or
  being flashed, GPIO3 floats and the node can stay in reset: it then does
  not appear as a USB device at all. Once the bridge runs, re-plug the node
  (holding BOOT for its first flash) and it enumerates (section 6).

## 2. Build

From the repo root (the first run installs ESP-IDF into `.tools/`, see
[build.md](build.md)):

```sh
powershell -ExecutionPolicy Bypass -File build.ps1 node_updater           # the updater (node)
powershell -ExecutionPolicy Bypass -File build.ps1 examples/bench_app      # the bench app (node, over CAN)
powershell -ExecutionPolicy Bypass -File build.ps1 examples/slcan_bridge   # the SLCAN bridge (brain)
./build.sh node_updater                                                    # Linux / macOS
./build.sh examples/bench_app
./build.sh examples/slcan_bridge
```

The bench app (`examples/bench_app/`) logs its version and OTA state at boot,
marks itself valid after a trivial self-test, and answers `u` + Enter on its
console by calling `can_updater_client_enter_updater()`. It sends no CAN
traffic of its own. Its OTA image is `examples/bench_app/build/bench_app.bin`.
For an application the script runs `idf.py app` (the app's own bootloader is
never flashed, so it is not built).

**The "bad" image for test (d)** skips `esp_ota_mark_app_valid_cancel_rollback()`
(Kconfig `BENCH_APP_SKIP_MARK_VALID`). It is built in its own build dir,
layering `sdkconfig.skip_mark_valid` on the normal defaults, so both images
exist side by side:

```sh
powershell -ExecutionPolicy Bypass -File build.ps1 examples/bench_app -B skip -D SDKCONFIG=skip/sdkconfig -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.skip_mark_valid" app
./build.sh examples/bench_app -B skip -D SDKCONFIG=skip/sdkconfig -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.skip_mark_valid" app
```

Result: `examples/bench_app/skip/bench_app.bin`. (Keep the directory name
short: on Windows, a longer one pushes object paths past `MAX_PATH`.)

The bench app's console is the **USB-Serial-JTAG** port (the primary
console, the only one that reads input). If you watch the node over UART0
instead, switch it with `build.ps1 examples/bench_app menuconfig` →
*Component config → ESP System Settings → Channel for console output*.

## 3. First flash over USB (once per board)

### The node: the updater

This writes the bootloader, partition table, `otadata` and the updater
(`factory`). `erase-flash` first clears whatever the board held before, so
`ota_0`/`ota_1` start empty and the `cancfg` block is unprovisioned (default
IDs `0x79E`/`0x79F`).

```sh
powershell -ExecutionPolicy Bypass -File build.ps1 node_updater -p COM7 erase-flash flash monitor
./build.sh node_updater -p /dev/ttyACM0 erase-flash flash monitor
```

or with esptool directly, from `node_updater/build/` (esptool 4.x, the version ESP-IDF
v5.3 installs):

```sh
python -m esptool --chip esp32s3 -p COM7 erase_flash
python -m esptool --chip esp32s3 -p COM7 -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 4MB --flash_freq 80m 0x0 bootloader/bootloader.bin 0x8000 partition_table/partition-table.bin 0xe000 ota_data_initial.bin 0x20000 can_updater.bin
```

esptool resets the S3 into download mode over USB-Serial-JTAG by itself. If
it cannot connect (observed when the board runs firmware that does not answer
esptool's reset), hold **BOOT** while plugging in the board's USB, release
it, and flash again. Observed on these boards: after a flash from a
BOOT-button download mode, esptool's "Hard resetting via RTS pin" can leave
the chip in download mode. Unplug and re-plug the USB **without** BOOT to
start the new firmware. Flash the brain before the node: while the brain is
in download mode its GPIO3 floats and holds the node in reset.

**Expected on the console** (the updater logs to UART0 and, as secondary
output, to USB-Serial-JTAG):

```
I (...) boot: Defaulting to factory image
I (...) boot: Loaded app from partition at offset 0x20000
...
I (...) can_updater: parked: cmd ID 0x79E, reply ID 0x79F, 250000 bit/s, slot ota_0 (1835008 bytes)
```

Keep `monitor` open on the node (`COM7`) through the tests: it reconnects
across resets (Ctrl+] quits; Ctrl+T then Ctrl+R resets the node). Reopening
it resets the node, unless you add `--no-reset` (`build.ps1 node_updater -p
COM7 monitor --no-reset`). To see the bench app's logs with its symbols, run the monitor
from its project: `build.ps1 examples/bench_app -p COM7 monitor`.

### The brain: the SLCAN bridge

```sh
powershell -ExecutionPolicy Bypass -File build.ps1 examples/slcan_bridge -p COM8 flash
./build.sh examples/slcan_bridge -p /dev/ttyACM1 flash
```

`flash` writes the bridge's own bootloader, partition table and app over the
brain's USB, replacing whatever the brain held. The bridge is a standalone
image, not an OTA image: built without `flash`, the scripts print its flash
offsets rather than an image to send over CAN. In a deep Windows checkout,
`flash` also builds the bridge's bootloader and CMake prints an
object-path-length warning for it; the build still completes.

The brain restarts as an SLCAN adapter on the same COM port. That port now
carries SLCAN and nothing else: do not open `monitor` on it (a COM port takes
one program at a time, and can-flash needs it). The bridge logs to UART0:

```
I (...) slcan_bridge: SLCAN on USB-Serial-JTAG (serial N1A2B); TWAI TX GPIO6, RX GPIO7, 250000 bit/s until the host sends Sn; GPIO3 (node reset) driven LOW
I (...) slcan_bridge: channel open at 250000 bit/s      <- each can-flash run
I (...) slcan_bridge: channel closed
```

Bus trouble and frame loss appear there as `W` lines, e.g. `bus
error-passive (TEC 128, REC 0)` or `... frame(s) lost: TWAI RX queue full`.

**Check the bridge** (the node is not needed):

```sh
uv run --with python-can --with pyserial python -c "import can; b = can.Bus(interface='slcan', channel='COM8', bitrate=250000); print(b.get_version(1), b.get_serial_number(1)); b.shutdown()"
```

Expected after about 2 s (python-can waits that long after opening a port):
`(1, 1) 1A2B`, where the serial number is the last two bytes of the brain's
MAC. `(None, None) None` means `COM8` is not the bridge: wrong port, or the
brain is not running it (if a UART0 log shows `waiting for download`, the
brain sits in its ROM download mode; power-cycle it).

## 4. Tests

Run `can_flash.py` from the repo root. `uv run` pulls its dependencies,
including pyserial for the `slcan` backend. `--channel COM8` is the brain;
substitute your port.

### (a) The node is visible: `--info` / `--scan`

```sh
uv run tools/can-flash/can_flash.py --info --interface slcan --channel COM8
uv run tools/can-flash/can_flash.py --scan --interface slcan --channel COM8
```

Expected (unprovisioned node, so the name is empty):

```
INFO: protocol v1, product name (0 chars): ''
```

```
Scanned N candidate cmd IDs (band 0x700..0x71F plus legacy 0x79E); found 1 parked board(s).

  #   NAME                             CMD_ID   REPLY_ID  PROTO
  1                                    0x79E    0x79F     v1
```

`no INFO reply after 4 attempts` or `found 0 parked board(s)` means no CAN
link: check wiring, termination, common ground, the bitrate (250 k), the
bridge check in section 3, and that the node's console shows the `parked:`
line.

### (b) Flash the bench app over CAN, GO, app confirms itself

```sh
uv run tools/can-flash/can_flash.py examples/bench_app/build/bench_app.bin --interface slcan --channel COM8 --bitrate 250000
```

Expected from can-flash (sizes and CRC vary with the build):

```
HELLO ok: protocol v1, app slot 1835008 bytes
SIZE ok: 191264 bytes
ERASE... (up to 15 s)
ERASE ok
DATA: 94 windows of up to 2048 bytes
  100.0%  191264/191264 bytes  ...s
VERIFY ok: CRC32 0x........
GO sent -- application starting
Done: 191264 bytes, CRC32 0x........, ... s
```

Expected on the node's console (the version is the repo's `git describe`):

```
I (...) boot: Loaded app from partition at offset 0x80000
I (...) bench_app: bench_app version v1.0.2-11-g....... (ESP-IDF v5.3.6), running from ota_0, OTA state PENDING_VERIFY
I (...) bench_app: self-test passed; image marked valid (rollback cancelled)
I (...) bench_app: type 'u' + Enter to enter the CAN updater
```

Reset the node (Ctrl+T Ctrl+R): it boots the bench app again, now with
`OTA state VALID`.

### (c) `u` on the console brings the updater back

Type `u` and Enter in the monitor:

```
I (...) bench_app: entering the CAN updater
...
I (...) boot: Defaulting to factory image
I (...) boot: Loaded app from partition at offset 0x20000
I (...) can_updater: parked: cmd ID 0x79E, reply ID 0x79F, 250000 bit/s, slot ota_0 (1835008 bytes)
```

`--info` answers again. Entering the updater erases `otadata`, so the node
now stays in the updater across resets until an image is flashed.

### (d) Rollback: a "bad" image lands back in the updater

With the node in the updater (after (c)), flash the SKIP_MARK_VALID build:

```sh
uv run tools/can-flash/can_flash.py examples/bench_app/skip/bench_app.bin --interface slcan --channel COM8 --bitrate 250000
```

can-flash ends as in (b). The node boots it once, unconfirmed:

```
I (...) bench_app: bench_app version ... running from ota_0, OTA state PENDING_VERIFY
W (...) bench_app: self-test passed, but this SKIP_MARK_VALID build leaves the image unconfirmed; the next reset rolls back to the CAN updater
```

Now reset the node (Ctrl+T Ctrl+R, the EN button, or a power cycle). The
bootloader marks the image `ABORTED`, finds no other valid OTA app, and
falls back to the updater:

```
I (...) boot: Defaulting to factory image
I (...) boot: Loaded app from partition at offset 0x20000
I (...) can_updater: parked: ...
```

Re-flash the good image (b) to get back to a confirmed app.

### (e) Power loss mid-transfer: the node comes back in the updater

With the node in the updater, start (b) and pull the node's power at around
50 %. can-flash fails, typically with:

```
error: DATA window N not accepted after 4 attempts
```

Restore power. `otadata` was never touched, so the node boots the updater
(`boot: Defaulting to factory image`, then `parked:`). Run (b) again: it
succeeds and the app confirms itself. (HELLO restarts the session, and ERASE
reopens the slot from the start.)

### (f) CAN unplugged mid-transfer: an error, then a clean retry

With the node in the updater, start (b) and disconnect CANH/CANL at around
50 %. Through the bridge can-flash fails with:

```
error: DATA window N not accepted after 4 attempts
```

(python-can's `slcan` writes never block: the bridge NAKs a frame it cannot
queue, and python-can ignores the NAK, so can-flash only notices the missing
ACKs.) Alone on the bus, the brain's frames go unacknowledged; its UART0 log
shows `bus error-passive` and `... frame(s) not sent`. The node keeps running
the updater (no reset).

Reconnect the bus and run (b) again: it succeeds. The bridge needs no
restart: can-flash's exit closed the channel (`C`), which stopped TWAI and
dropped the frames still queued, and the next run reopens it. Bus-off, if it
happens, recovers on its own.

## 5. Recovery until Phase 3

There is no GPIO recovery: GPIO4 is TWAI TX, and the bootloader would sample
the transceiver's D line at reset. Software recovery (an RTC crash-loop
counter, plus a flash boot counter the brain trips with EN pulses) arrives in
Phase 3. Until then the ways into the updater are: a fresh board, the app
calling `can_updater_client_enter_updater()` (`u` here), or rollback of an
app that never marks itself valid.

If a confirmed app on the bench cannot reach the updater any more, erase
`otadata` over USB. The bootloader then falls back to `factory` and nothing
else is rewritten:

```sh
python -m esptool --chip esp32s3 -p COM7 erase_region 0xe000 0x2000
```

## 6. Notes for the brain's reset line to the node

The brain (ESP32-S3-WROOM-1U-N16R8, TWAI TX GPIO6 / RX GPIO7) controls the
node's EN (reset) from its **GPIO3**. That is the only brain-to-node wire
besides CAN and ground. Reset pulses arrive with Phase 3.

- **Polarity is inverted: GPIO3 HIGH = node held in reset, LOW = node runs.**
  Firmware on the brain drives GPIO3 push-pull LOW as early as possible
  (latching the level before enabling the output, so it never glitches high)
  and pulses it HIGH only to reset the node.
- **A floating GPIO3 can hold the node in reset.** While the brain is blank,
  in ROM download mode, being flashed or still booting, GPIO3 is not driven;
  on these boards that has been observed to keep the node in reset (no USB
  enumeration). Flash the brain first, then plug in or reset the node.
- **GPIO3 is an ESP32-S3 strapping pin, but only if the `STRAP_JTAG_SEL`
  eFuse is burned** (it selects the JTAG source at reset). On a default chip
  it is not burned, so the level on GPIO3 at the brain's own reset does not
  matter. Leave that eFuse alone.

## 7. Alternative: a USB-CAN adapter instead of the brain

Any python-can adapter can stand in for the brain: CANable/slcan, PCAN,
Kvaser, gs_usb, SocketCAN, …. Wire its CANH, CANL and GND to the node as in
section 1 with the adapter in the brain's place, and put the second 120 Ω at
the adapter (most have a jumper or switch for it). Run the same tests with
its interface and channel, e.g.:

```sh
uv run tools/can-flash/can_flash.py --info --interface slcan --channel COM9   # CANable (slcan firmware)
uv run tools/can-flash/can_flash.py --info --interface pcan --channel PCAN_USBBUS1
sudo ip link set can0 up type can bitrate 250000                                            # Linux SocketCAN, then:
uv run tools/can-flash/can_flash.py --info --interface socketcan --channel can0
```

Test (f) differs with an adapter: can-flash may also fail with `error: local
CAN TX queue stayed full for 2 s -- frames are not leaving the interface
...`, and an adapter left error-passive or bus-off may need a restart before
the retry (SocketCAN: `ip link set can0 down && ip link set can0 up type can
bitrate 250000`; otherwise unplug and replug it).

## 8. Brain OTA over Ethernet (issue #5)

`brain/` (firmware `brain_ota`) lets the **brain** update itself over its **W5500**
Ethernet port: an image POSTed over HTTP goes into the inactive OTA slot,
the brain reboots into it, and a self-test marks it valid or rolls it back.
The node, CAN and can-flash play no part here. Wi-Fi/BLE follow in issue
#4: the OTA core (`brain/main/ota_update.c`) takes image chunks from any
transport, and the HTTP handler is just one caller.

| Item | Value |
|---|---|
| W5500 | SPI2 (IOMUX): MOSI GPIO11, MISO GPIO13, SCLK GPIO12, CS GPIO10, INTn GPIO9, RSTn GPIO14; SPI at 20 MHz |
| Network | DHCP client; MAC = the chip's Ethernet MAC (base MAC + 3) |
| HTTP | port 80: `GET /` (plain-text status), `POST /ota` (raw `.bin` as the body, `Content-Length` required) |
| Self-test | a fresh image (`PENDING_VERIFY`) must get an IP within **30 s**, then marks itself valid; otherwise it rolls back and reboots |
| Console | UART0 (GPIO43/44) and, as secondary output, USB-Serial-JTAG (the brain's USB port, **COM63** below) |

Partition table (`brain/partitions.csv`, 16 MB, no factory app):

| Partition | Offset | Size | Contents |
|---|---|---|---|
| bootloader | `0x0` | — | stock ESP-IDF 2nd stage, app rollback enabled |
| partition table | `0x8000` | 4 KB | |
| `nvs` | `0x9000` | 20 KB | |
| `otadata` | `0xe000` | 8 KB | boot-slot selection and image states |
| `phy_init` | `0x10000` | 4 KB | |
| `ota_0` | `0x20000` | 3 MB | app slot A (the USB flash lands here) |
| `ota_1` | `0x320000` | 3 MB | app slot B |
| `node_fw` | `0x620000` | 2 MB | disposable staged node image (custom data subtype `0x41`); section 9 |

`0x820000`–`0x1000000` (7.9 MB) is free.

**While brain_ota runs, the brain is not the SLCAN bridge.** Sections 3–4
need the bridge: re-flash it over USB to return to CAN bench work
(`build.ps1 examples/slcan_bridge -p COM63 flash`, with the brain's port).

### 8.1 Build

```sh
powershell -ExecutionPolicy Bypass -File build.ps1 brain
./build.sh brain
```

(`build.ps1` / `./build.sh` with no argument build it too, after the node
updater.) brain_ota has its own partition table, so the scripts run a full
`build` and print its flash offsets (bootloader `0x0`, table `0x8000`,
otadata `0xe000`, app `0x20000`). The OTA image is
`brain/build/brain_ota.bin` (about 440 KB).

**The "bad" image for test (2)** skips `esp_ota_mark_app_valid_cancel_rollback()`
(Kconfig `BRAIN_OTA_SKIP_MARK_VALID`). It is built in its own short build
dir, as for the bench app:

```sh
powershell -ExecutionPolicy Bypass -File build.ps1 brain -B skip -D SDKCONFIG=skip/sdkconfig -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.skip_mark_valid" app
./build.sh brain -B skip -D SDKCONFIG=skip/sdkconfig -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.skip_mark_valid" app
```

Result: `brain/skip/brain_ota.bin`.

### 8.2 First flash over USB

Connect the brain's Ethernet to the same DHCP network as the PC, and its USB
port to the PC, then:

```sh
powershell -ExecutionPolicy Bypass -File build.ps1 brain -p COM63 flash monitor
./build.sh brain -p /dev/ttyACM0 flash monitor
```

esptool resets the brain into download mode through USB-Serial-JTAG by
itself. If it cannot connect, hold BOOT (GPIO0) while pressing reset and
retry. `flash` writes the bootloader, the 16 MB table, a blank `otadata` and
the app into `ota_0`, replacing whatever the brain held (the bridge
included); add `erase-flash` before `flash` for a clean slate. `monitor`
reconnects across resets (Ctrl+] quits; Ctrl+T then Ctrl+R resets the
brain); `build.ps1 brain -p COM63 monitor` reopens it later.

Expected on the console. The version is the repo's `git describe`; the
2nd-stage bootloader's `boot:` lines may appear on UART0 only.

```
I (...) brain_ota: GPIO3 (node reset) driven LOW
I (...) brain_ota: brain_ota version v1.0.2-... (ESP-IDF v5.3.6), running from ota_0 (0x20000), OTA state VALID
I (...) eth_w5500: W5500 on SPI2 at 20 MHz (MOSI 11, MISO 13, SCLK 12, CS 10, INT 9, RST 14), MAC xx:xx:xx:xx:xx:xx
I (...) eth_w5500: Ethernet started; waiting for link and DHCP
I (...) http_ota: HTTP server on port 80: GET /, POST /ota
I (...) eth_w5500: link up
I (...) eth_w5500: ==================================================
I (...) eth_w5500: IP acquired: 192.168.1.57   ->  http://192.168.1.57/
I (...) eth_w5500: ==================================================
```

On the first boot after a USB flash `otadata` is blank; the bootloader
records `ota_0` as VALID, so no self-test runs. **Note the IP** from the
`IP acquired` line (or find the MAC from the `W5500` line in your router's
DHCP table). Below, `<ip>` stands for it.

`W5500 driver install failed`, or no `link up`, means a wiring, reset or
cable problem. Nothing else in this section works until `IP acquired` shows.

### 8.3 Status and upload

```sh
curl.exe http://<ip>/
```

```
project:   brain_ota
version:   v1.0.2-...
esp-idf:   v5.3.6
running:   ota_0 (0x20000)
ota state: VALID
next slot: ota_1
ip:        192.168.1.57
uptime:    42 s
```

Upload an image. In PowerShell write `curl.exe`: plain `curl` is an alias
for `Invoke-WebRequest` there.

```sh
curl.exe --data-binary "@brain/build/brain_ota.bin" http://<ip>/ota
```

`--data-binary` sends the file unmodified, with a `Content-Length` header.
Expect a few seconds, not well under one: the 100 Mbit link is not the
bottleneck. Erasing and writing the flash sector by sector is, with the
20 MHz SPI to the W5500 after it.

On success the reply is `OTA ok: N bytes; rebooting into the new image`
(HTTP 200) and the brain restarts about 1 s later. Every error leaves the
boot partition unchanged:

| Status | Cause |
|---|---|
| 411 | no `Content-Length` (e.g. a chunked upload) |
| 413 | image larger than the 3 MB slot |
| 409 | the running image is still `PENDING_VERIFY`, or an OTA/node upload/node flash/scan is active |
| 400 | not an app image, or rejected by validation (`ESP_ERR_OTA_VALIDATE_FAILED`) |
| 408 | no data for 10 s mid-upload (the reply may not get out) |
| 500 | flash error |

**Only upload brain_ota images.** Any valid ESP32-S3 app is accepted and
booted. One that confirms itself but has no Ethernet (the bench app, for
instance) can then only be replaced over USB.

### 8.4 Tests

Keep `monitor` open on COM63 through the tests. Byte counts below are for a
449 568-byte image.

**Opening COM63 can reset the brain.** Observed on hardware: a tool that
(re)opens the port with pyserial's default DTR/RTS (a plain script, or
`monitor` without `--no-reset`) resets the S3. During test (2) that reset
alone triggers the rollback before you mean it to. Either keep one `monitor`
session open the whole time, or watch the state with `curl.exe http://<ip>/`
instead of the console.

Results on the real brain (2026-10-14, 172.16.5.71): (1) 449 568 B in about
3.3 s, `PENDING_VERIFY` → VALID; (2) bad image `PENDING_VERIFY`, a second
upload refused with 409, reset → rolled back, VALID; (3) cable pulled mid
upload → no reboot, still the old slot, VALID; (4) power cut mid upload →
old slot, VALID.

#### (1) Good image: reboot, self-test, VALID

```sh
curl.exe --data-binary "@brain/build/brain_ota.bin" http://<ip>/ota
```

```
I (...) http_ota: POST /ota: 449568 bytes announced
I (...) ota_update: OTA begin: 449568 bytes into ota_1 (offset 0x320000, 3145728 bytes)
I (...) ota_update: OTA progress:  10 % (... / 449568 bytes)
...
I (...) ota_update: OTA progress: 100 % (449568 / 449568 bytes)
I (...) ota_update: OTA result: 449568 bytes written and validated; ota_1 boots next
I (...) http_ota: restarting in 1000 ms
...
I (...) brain_ota: brain_ota version ... running from ota_1 (0x320000), OTA state PENDING_VERIFY
I (...) brain_ota: self-test: new image, waiting up to 30 s for an IP
I (...) eth_w5500: IP acquired: ...
I (...) brain_ota: self-test passed; image marked VALID (rollback cancelled)
```

`curl.exe http://<ip>/` now shows `running: ota_1` and `ota state: VALID`.
Reset the brain (Ctrl+T Ctrl+R): it boots `ota_1` again, `OTA state VALID`.
Each further upload goes to the other slot.

#### (2) Bad (SKIP_MARK_VALID) image: rollback on the next reset

```sh
curl.exe --data-binary "@brain/skip/brain_ota.bin" http://<ip>/ota
```

It is written and booted as in (1), but stays unconfirmed:

```
I (...) brain_ota: brain_ota version ... running from ota_0 (0x20000), OTA state PENDING_VERIFY
I (...) brain_ota: self-test: new image, waiting up to 30 s for an IP
I (...) eth_w5500: IP acquired: ...
W (...) brain_ota: self-test passed, but this SKIP_MARK_VALID build leaves the image unconfirmed; the next reset rolls back to the previous slot
```

`GET /` shows `ota state: PENDING_VERIFY`, and an upload now gets `409`.
Reset the brain (Ctrl+T Ctrl+R, the reset button, or a power cycle). The
bootloader marks the unconfirmed slot `ABORTED` and boots the previous one:

```
I (...) brain_ota: brain_ota version ... running from ota_1 (0x320000), OTA state VALID
```

A fresh image that gets no IP within 30 s (booted with the cable out, say)
rolls back by itself, without a reset:

```
E (...) brain_ota: self-test failed (no network within 30 s); rolling back and rebooting
```

#### (3) Ethernet cable pulled mid-upload: abort, old image still boots

Slow the upload down so there is time to act. At 8 KB/s the image takes
about 55 s; `--limit-rate 20k` (about 22 s) also works if you are quick.

```sh
curl.exe --limit-rate 8k --data-binary "@brain/build/brain_ota.bin" http://<ip>/ota
```

Pull the brain's Ethernet cable at around 50 %:

```
I (...) ota_update: OTA progress:  50 % (... / 449568 bytes)
W (...) eth_w5500: link down
E (...) http_ota: no data for 10 s after ... of 449568 bytes (cable pulled?)
W (...) ota_update: OTA result: aborted after ... of 449568 bytes; boot partition unchanged
```

curl does not notice at once (TCP keeps retrying); stop it with Ctrl+C.
Plug the cable back in (`link up`, then `IP acquired` again). `GET /` shows
the same running slot, `VALID`; a reset boots that same slot; a new upload
succeeds.

#### (4) Power cut mid-upload: old image still boots

Start the slowed upload as in (3) and cut the brain's power (unplug its USB)
at around 50 %. curl fails. Restore power: `otadata` was never touched, so
the brain boots the slot it ran before, `OTA state VALID`, and gets its IP
again. The half-written slot is overwritten by the next upload.

### 8.5 GPIO3 and the node during brain reboots

brain_ota drives GPIO3 (the node's reset; HIGH holds the node in reset) LOW
first thing in `app_main`, through the same `components/node_reset` as the
bridge. From each brain reset until
then (the bootloader plus start-up, well under a second) GPIO3 floats, so
**every OTA reboot, rollback and reset of the brain can hold the node in
reset briefly, and the node restarts.** That is accepted for this bench. A
node connected over USB re-enumerates.

## 9. Brain flashes the node over CAN (Phase 2, issue #8)

**Bench-only, trusted LAN:** these HTTP routes have no authentication or TLS.
The PC supplies the image over Ethernet; it is **not on the CAN bus**. The
brain runs `brain_ota`, not the SLCAN bridge. The node must already be parked
in its updater: use the bench app's `u` command or section 5's USB recovery.
This phase neither remotely enters the updater nor pulses reset. **GPIO3
stays LOW throughout staging, scanning and flashing.** Brain reboot caveats
in section 8.5 still apply.

### 9.1 Prepare and discover

Build both firmwares and the node app, then flash the **brain first** over
USB (replace COM63 with its actual port):

```sh
powershell -ExecutionPolicy Bypass -File build.ps1
powershell -ExecutionPolicy Bypass -File build.ps1 examples/bench_app
powershell -ExecutionPolicy Bypass -File build.ps1 brain -p COM63 flash
```

If BOOT was held for download mode and it stays there after flashing,
power-cycle without BOOT. Let the brain acquire a DHCP address as in
section 8.2. Put the node into its updater; if it has never been installed,
follow section 3's node USB flash, using the node's actual port. Connect
CANH/CANL/GND and the two 120-ohm terminators as in section 1.

```sh
curl.exe http://<ip>/node/scan
curl.exe "http://<ip>/node/scan?from=0x700&to=0x73F"
```

Default scan: sorted unique `0x700..0x71F`, plus `0x79E` and known command
IDs `1,3,5,7,9`. Overrides replace only the range; the extra IDs are always
probed. Each probe has a **300 ms total INFO deadline**, listens on any
standard reply ID, and requires the header plus four name frames on the
same ID. An empty/default scan takes about **11.4 s**. Multiple parked nodes
need distinct command IDs and unambiguous reply IDs. With one unprovisioned
node, expect (IDs are decimal in JSON):

```json
{"nodes":[{"cmd_id":1950,"reply_id":1951,"name":"","protocol":1}],"truncated":false}
```

The brain stores at most 64 scan results; `truncated:true` asks you to narrow
the range. Names are JSON-escaped (including embedded control characters).
Bad ranges/IDs return 400. Scans share the update worker, so another scan or
either update returns 409 while one runs; status endpoints remain live.

### 9.2 Stage, flash, verify and boot

```sh
curl.exe -i --data-binary "@examples/bench_app/build/bench_app.bin" "http://<ip>/node/flash?cmd=0x79E&reply=0x79F"
curl.exe http://<ip>/node/status
```

The same IDs are defaults if the query is omitted. For a provisioned node,
use **both IDs from scan**; the reply need not be command + 1. Decimal and
`0x` hexadecimal are accepted. Send the app `.bin`, never a bootloader or
merged flash image.

The body must have a nonzero Content-Length. The brain erases `node_fw`
sector by sector, writes each chunk and reads it back for byte equality.
**202 Accepted is returned after the complete body is verified in staging,
without waiting for CAN flashing.** A static background worker then runs
HELLO → SIZE → ERASE → DATA → VERIFY → GO from that partition. Poll status:

```json
{"state":"flashing","percent":42,"bytes":81920,"total":191264,"status":"CAN transfer; awaiting VERIFY and GO"}
```

States: `idle`, `staging`, `flashing`, `success`, `failed`. Bytes/percent refer
to staged bytes while staging and accepted image bytes while flashing;
**100% is not success until VERIFY and GO are ACKed.** On success expect
`state:success`, `percent:100`, equal `bytes`/`total`, and `status` beginning
`GO: success`. On the node's console expect the normal bench-app boot and
`self-test passed; image marked valid`, as in section 4(b). The brain does
not reboot and its own running slot is unchanged. A running bench app no
longer answers INFO; use `u` again before the next transfer.

### 9.3 Failure and isolation checks (run only on an available bench)

- **Early replies on Windows:** the brain answers 409/411/413 as soon as it
  reads the headers, then closes. With a large body still being sent, Windows
  curl may report `000` / "Connection was reset" and drop the reply; `-v`
  shows the status line, or send a small body to see it cleanly.
- **Conflicts, both directions:** start a slow node upload with
  `curl.exe --limit-rate 8k --data-binary "@examples/bench_app/build/bench_app.bin" "http://<ip>/node/flash"`.
  In another terminal POST again to `/node/flash`, then POST the brain image
  to `/ota`; each must return **409**, during staging and during CAN flashing.
  `/node/status` must still respond. Repeat with a slow `/ota` upload and a
  concurrent `/node/flash`: again 409. Abort the slow self-upload before
  completion if you do not intend to reboot the brain.
- **Dropped upload:** cancel curl or pull Ethernet during staging. After the
  10 s receive timeout (or immediate peer close), status is `failed`; no CAN
  session starts, neither boot selection changes, and a fresh upload works.
  Missing length returns 411; a body larger than the **2 MB staging partition**
  returns 413 before erase. A staged image bigger than the node's announced
  **1.75 MB slot** gets `SIZE rejected: size out of bounds 0x04`, before node
  ERASE. A complete upload whose 202 response cannot be sent is not flashed.
- **No node / CAN disconnected:** scan returns no hits on a healthy bus, or a
  transport error if TX cannot drain. Flash ends `failed` with a HELLO timeout
  or transport failure, never success. Disconnect CAN mid-DATA, then reconnect
  and retry the entire HTTP upload after failure. Inspect the brain's final
  `CAN health` log for bus state, TX/RX error counters and dropped frames.
- **Node reset/power loss mid-transfer:** the node returns to its updater;
  this attempt fails with a clear DATA/transport status. A fresh POST starts
  a new HELLO session and succeeds. No automatic reset is driven by the brain.
- **Rejected image:** a body whose first byte is not the ESP image magic `0xE9`
  is refused by the node's first `esp_ota_write`, so it ends `failed` with
  `DATA window 0 not accepted after 4 attempts`. To reach the VERIFY check,
  upload garbage that starts with `0xE9` (e.g. 4 KB random with the first byte
  replaced): it ends `failed` with `VERIFY rejected: image rejected 0x06`.
  Either way the node stays in its updater. CRC mismatch is independently
  covered by host fault tests.
- **Brain power loss during staging:** after reboot, status is `idle`. Length
  and validity are RAM-only, so partial/stale `node_fw` contents are never
  resumed. During CAN flashing, power loss leaves the node in its updater
  unless GO already selected the verified app. Re-upload to start fresh.
- **Own OTA regression:** repeat section 8.4's normal image, skip-mark-valid
  rollback, dropped upload and power-cut checks. The self-test and A/B OTA
  implementation are unchanged; a single async HTTP worker now keeps conflict
  and status replies responsive while either upload is active.

### 9.4 Host compatibility and known limits

`components/can_flash_host/include/can_flash_host.h` documents the portable
C API and typed diagnostics. The host tests run against the **real**
`src/boot/can_update.c`, not a permissive Python fake. Four attempts, 2 s
ordinary ACK, 15 s ERASE, 150 ms quiet drain, 2048-byte windows and current/
next-window rewind resolution match the Python defaults. The ESP-IDF adapter
rounds the 1 ms early receive poll up to one RTOS tick so it actually yields.
A continuously busy receive drain is bounded at 2 s rather than hanging.

Intentional differences / protocol limits:

- The Python `_command` calls commands idempotent; the real engine accepts
  SIZE, ERASE and VERIFY only in their exact phase. Losing those ACKs can
  cause a retry to get STATE. Restart the full upload rather than treating
  that NAK as success. A lost GO ACK is ambiguous: the app may already run.
- The Python host retries a lost final DATA ACK, but the real node is already
  TRANSFERRED and rejects DATA. The C host first probes VERIFY on a final
  DATA timeout: ACK proves delivery and CRC; STATE safely permits DATA retry;
  CRC/image rejection fails closed. It does not send VERIFY again after ACK.
- SIZE is sent even if the announced slot is smaller, obtaining the node's
  authoritative SIZE NAK rather than a Python-side size exception. No ERASE
  follows a rejected SIZE.
- INFO's five frames are unnumbered: an incomplete/foreign burst is retried
  (or a scan miss), not guessed. Provisioned names are ASCII, at most 32 bytes.

Host verification (no hardware):

```sh
cmake -S tests -B build-host
cmake --build build-host --config Debug
ctest --test-dir build-host -C Debug --output-on-failure
```

The suite includes >256 KB/wrap recovery, lost/duplicate DATA, lost intermediate
and final ACKs, final-frame loss, node reset/restart, CRC/image rejection,
size/timeout errors, three-node discovery and every name length 0..32. A
5099-byte deterministic image was cross-checked against the actual Python
`CanFlasher`: all **857 frames / 6856 payload bytes** (HELLO through GO,
including padding and sequence bytes) matched, CRC32 `0xBBCFA65A`.
