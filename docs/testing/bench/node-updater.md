# Bench: node CAN updater from a PC

Tests the **node**'s CAN updater from a PC with `tools/can-flash`, using the
**brain** board as the USB-CAN adapter: the brain runs the SLCAN bridge
(`examples/slcan_bridge`), so python-can talks to it as an ordinary `slcan`
adapter. The node is the custom **ESP32-S3FH4R2** board (4 MB in-package
flash) with a **TCAN332DR** on TWAI **TX = GPIO4, RX = GPIO5** under its
ODU profile; the brain is
the **ESP32-S3-WROOM-1U-N16R8** board with a TCAN332DR on **TX = GPIO6,
RX = GPIO7**. The bus runs at **250 kbit/s**. A commercial USB-CAN adapter
works in the brain's place ([below](#alternative-a-usb-can-adapter-instead-of-the-brain)).

History: Phase 0, issues #2 and #3. Common setup and what you need:
[README.md](README.md).

## 1. Wiring

As in [wiring.md, brain ↔ node](../../getting-started/wiring.md#brain--node):
CANH, CANL, GND, 120 Ω at each end. The bridge firmware drives GPIO3 LOW from
boot, so flash the bridge onto the brain first; then re-plug the node
(holding BOOT for its first flash) and it enumerates.

## 2. Build

From the repo root (the first run installs ESP-IDF into `.tools/`, see
[build.md](../../getting-started/build.md)):

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
never flashed, so it is not built). Its console is the USB-Serial-JTAG port
([configuration.md](../../reference/configuration.md#bench-app-examplesbench_app--bench-app)).

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

## 3. First flash over USB (once per board)

1. **The brain: the SLCAN bridge** —
   [slcan-bridge.md, flash it](../../guides/slcan-bridge.md#flash-it), then
   [check it](../../guides/slcan-bridge.md#check-it) (`(1, 1) 1A2B`).
2. **The node: the updater** —
   [first-flash.md, the node](../../getting-started/first-flash.md#the-node-the-updater).
   The commands below assume a **legacy unprovisioned v2 block** and default
   IDs `0x79E/0x79F`; a SOLO ODU preset instead uses `0x05/0x06` and product
   `SOLO ODU Controller`. For a provisioned ODU, pass
   `--id-tx 0x05 --id-rx 0x06` to can-flash commands and expect its name.

Read the updater's `parked:` line on **UART0**. Its TinyUSB CDC port is not
a console. The separate bench app still uses its own USB console for `u` and
logs. Reopening an app monitor can reset the board; use `--no-reset` when
appropriate. To see the bench app's logs with symbols, run the monitor from
its project: `build.ps1 examples/bench_app -p COM7 monitor`.

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
bridge check in step 3, and that the node's console shows the `parked:`
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

## 5. Recovery

All ways back into the updater (crash-loop and EN-pulse counters, `u`,
rollback, the USB `otadata` erase on `COM7`):
[recovery.md](../../concepts/recovery.md). Recovery tests:
[node-recovery.md](node-recovery.md).

## One updater, board profile and USB checklist (not yet run)

**Status: not run.** This checks issue #20, not the earlier 4 MB node pass.
Use current local universal-bundle binaries or the planned
`c-cpp-esp32-v1.2.0` release once published. Record bundle version, board,
port, flash size, profile bytes, UART0 log and measured GPIO levels for each
step. Do not run destructive flash steps while either board is serving an
update. See [provisioning](../../guides/provisioning-can-ids.md),
[profile vectors](../../reference/board-profile.md#solo-presets-and-golden-bytes)
and [first flash](../../getting-started/first-flash.md).

- [ ] Provision ODU `node-4MB` and IDU `idu-16MB` through their own USB ports
      using the **same** `bootloader.bin` and `can_updater.bin`; verify selected
      table, `--flash-size 4MB`/`16MB`, factory offset `0x20000`/`0xFA0000`
      and full 4 KB `cancfg` read-back against each preset's golden bytes.
- [ ] With no app installed, power-cycle each board. Confirm factory boot,
      UART0 `TWAI profile record` with ODU 4/5 and IDU 6/7 at 250 kbit/s,
      and CAN INFO on `0x05/0x06` and `0x07/0x08` respectively. Verify the
      IDU GPIO3 hold is LOW from 2nd-stage initialization, before partition
      logging, and remains LOW in updater. Use a scope for the startup timing;
      GPIO3 may float during ROM boot.
- [ ] Enumerate each updater's native USB: VID:PID `303A:1001`, manufacturer
      `Normal Corporation`, product `SOLO ODU Controller` or
      `SOLO IDU Controller`, and a 12-uppercase-hex MAC serial. Confirm logs
      appear on UART0, not on the updater's CDC port.
- [ ] Open the updater CDC port at 1200 baud and close it. Check the board
      restarts into ROM download with **Espressif** USB identity, then esptool
      connects without holding BOOT. Reset normally back into the updater.
- [ ] On a disposable install, test an erased or CRC-corrupt profile without
      damaging the v2 block. Confirm all profile defaults (TX4/RX5, 250k,
      no holds), record/default log, and continued USB enumeration. Restore
      the board preset afterward.
- [ ] With a controlled TWAI-init failure on valid nondefault profile pins,
      verify a single retry on TX4/RX5 while holds remain asserted. Repeat
      with a valid profile that holds GPIO4 or GPIO5 LOW: the updater must
      **skip** default retry and keep USB available without CAN. Do not use
      refused GPIOs or hold a TWAI pin in the profile; restore preset after.
- [ ] Install a matching IDU app and check APP_INFO/ENTER_UPDATER on its
      `0x07/0x08` pair as well as its brain role. Check an ODU app on
      `0x05/0x06`; test rollback/re-entry without overwriting either factory
      slot. If using the stock Arduino helper, confirm delayed image marking
      and flash-counter clear after stable uptime separately.

No checkmark above is evidence of a pass until it has been run and recorded.


## Alternative: a USB-CAN adapter instead of the brain

Any python-can adapter can stand in for the brain: CANable/slcan, PCAN,
Kvaser, gs_usb, SocketCAN, …. Wire its CANH, CANL and GND to the node as in
[wiring.md](../../getting-started/wiring.md#brain--node) with the adapter in
the brain's place, and put the second 120 Ω at the adapter (most have a
jumper or switch for it). Run the same tests with its interface and channel,
e.g.:

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
