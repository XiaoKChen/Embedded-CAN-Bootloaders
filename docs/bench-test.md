# Bench Test — ESP32-S3 Node + CAN Updater (Phase 0, issue #2)

Tests the **node** on its own, before the brain exists: a PC with a USB-CAN
adapter (python-can) plays the brain, using `tools/can-flash`. The node is the
custom **ESP32-S3FH4R2** board (4 MB in-package flash) with a **TCAN332DR**
on TWAI **TX = GPIO4, RX = GPIO5**, at **250 kbit/s**.

You need:

- the node board, a USB cable to its USB-Serial-JTAG port (GPIO19/20);
- a python-can adapter (CANable/slcan, PCAN, Kvaser, gs_usb, SocketCAN, …);
- `uv` (or Python 3.9+ with `tools/can-flash/requirements.txt`).

The commands below use a CANable on Windows (`--interface slcan --channel
COM8`) and the node's USB port as `COM7`. Substitute your own, e.g.
`--interface pcan --channel PCAN_USBBUS1`, or `--interface socketcan
--channel can0` and `/dev/ttyACM0` on Linux.

## 1. Wiring

```
ESP32-S3FH4R2 node          TCAN332DR                     USB-CAN adapter
GPIO4 (TWAI TX) ──────────► D                CANH ──────────── CANH
GPIO5 (TWAI RX) ◄────────── R                CANL ──────────── CANL
3V3 ─────────────────────── VCC                                  │
GND ─────────────────────── GND ─────────── GND (common ground) ─┘
                             120 Ω CANH–CANL here ... and 120 Ω at the adapter
```

- **3.3 V** supply on the transceiver's VCC (the TCAN332 is a 3.3 V part, so
  R is safe to feed straight into GPIO5).
- **Termination: 120 Ω at both ends** of the bus: one at the node, one at the
  adapter (most adapters have a jumper or switch for it). With everything
  unpowered, CANH–CANL should measure about **60 Ω**. If the node board
  already carries a terminator, do not add a second one at that end.
- **Common ground** between the node and the adapter. Without it the bus
  works intermittently or not at all.

## 2. Build

From the repo root (the first run installs ESP-IDF into `.tools/`, see
[build.md](build.md)):

```sh
powershell -ExecutionPolicy Bypass -File build.ps1                      # the updater
powershell -ExecutionPolicy Bypass -File build.ps1 examples/bench_app   # the bench app
./build.sh                                                              # Linux / macOS
./build.sh examples/bench_app
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

This writes the bootloader, partition table, `otadata` and the updater
(`factory`). `erase-flash` first clears whatever the board held before, so
`ota_0`/`ota_1` start empty and the `cancfg` block is unprovisioned (default
IDs `0x79E`/`0x79F`).

```sh
powershell -ExecutionPolicy Bypass -File build.ps1 -p COM7 erase-flash flash monitor
./build.sh -p /dev/ttyACM0 erase-flash flash monitor
```

or with esptool directly, from `build/` (esptool 4.x, the version ESP-IDF
v5.3 installs):

```sh
python -m esptool --chip esp32s3 -p COM7 erase_flash
python -m esptool --chip esp32s3 -p COM7 -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 4MB --flash_freq 80m 0x0 bootloader/bootloader.bin 0x8000 partition_table/partition-table.bin 0xe000 ota_data_initial.bin 0x20000 can_updater.bin
```

esptool resets the S3 into download mode over USB-Serial-JTAG by itself. If
it cannot connect, hold GPIO0 low while pulsing EN (if your board exposes
GPIO0) and retry.

**Expected on the console** (the updater logs to UART0 and, as secondary
output, to USB-Serial-JTAG):

```
I (...) boot: Defaulting to factory image
I (...) boot: Loaded app from partition at offset 0x20000
...
I (...) can_updater: parked: cmd ID 0x79E, reply ID 0x79F, 250000 bit/s, slot ota_0 (1835008 bytes)
```

Keep `monitor` open through the tests: it reconnects across resets
(Ctrl+] quits; Ctrl+T then Ctrl+R resets the node). Reopening it resets the
node, unless you add `--no-reset` (`build.ps1 -p COM7 monitor --no-reset`).
To see the bench app's logs with its symbols, run the monitor from its
project: `build.ps1 examples/bench_app -p COM7 monitor`.

## 4. Tests

Run `can_flash.py` from the repo root. `uv run` pulls its dependencies.

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
link: check wiring, termination, common ground, the adapter bitrate (250 k),
and that the console shows the `parked:` line.

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
uv run tools/can-flash/can_flash.py examples/bench_app/skip/bench_app.bin --interface slcan --channel COM8
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
50 %. can-flash fails, depending on the adapter, with one of:

```
error: DATA window N not accepted after 4 attempts
error: local CAN TX queue stayed full for 2 s -- frames are not leaving the interface ...
```

The node keeps running the updater (no reset). Reconnect the bus. If the
adapter reports an error-passive or bus-off state, restart it (SocketCAN:
`ip link set can0 down && ip link set can0 up type can bitrate 250000`;
otherwise unplug and replug it). Run (b) again: it succeeds.

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

## 6. Notes for the brain hardware (no code in this phase)

The brain (ESP32-S3-WROOM-1U-N16R8, TWAI TX GPIO6 / RX GPIO7) drives the
node's EN (reset) pin from its **GPIO3**. That is the only brain-to-node wire
besides CAN.

- **Drive GPIO3 open-drain, pull-low only.** To reset the node, drive it low;
  otherwise release it (high-Z), never drive it high. A push-pull high would
  fight the node's EN RC pull-up and override its power-on delay.
- **The node's EN must have its own pull-up (the RC network).** While the
  brain boots, or is itself held in reset, its GPIO3 floats and the node must
  keep running on that pull-up alone.
- **GPIO3 is an ESP32-S3 strapping pin, but only if the `STRAP_JTAG_SEL`
  eFuse is burned** (it selects the JTAG source at reset). On a default chip
  it is not burned, so the level on GPIO3 at the brain's own reset does not
  matter. Leave that eFuse alone.
