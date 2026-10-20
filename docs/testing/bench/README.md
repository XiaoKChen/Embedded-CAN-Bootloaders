# Bench tests

Hardware procedures for the brain + node bench. **Run hardware steps only when
the bench is available.** Building/testing a change does not itself flash
boards, open serial ports, or run these tests. HTTP is trusted-LAN-only.

| Guide | Tests | Brain runs | History |
|---|---|---|---|
| [node-updater.md](node-updater.md) | existing 4 MB node CAN test plus **not-yet-run** one-updater board-profile/USB checklist for ODU and IDU | SLCAN bridge (or USB-CAN adapter) | Phase 0, #2, #3; #20 pending |
| [brain-ota.md](brain-ota.md) | the brain updating itself over Ethernet | `brain_ota` | #5 |
| [brain-can-flash.md](brain-can-flash.md) | the brain flashing a parked node over CAN (`/node/scan`, `/node/flash`) | `brain_ota` | Phase 2, #8 |
| [node-recovery.md](node-recovery.md) | crash-loop and brain-forced node recovery | `brain_ota` | Phase 3, #9 |
| [update-package.md](update-package.md) | brain-first update package and resumable node relay | `brain_ota` | Phase 5, #10 |
| [node-app.md](node-app.md) | the reference node app, parked over CAN without GPIO3 | `brain_ota` | Phase 6, #11 |

## Issue #20: pending bench validation

The [new checklist](node-updater.md#one-updater-board-profile-and-usb-checklist-not-yet-run)
checks the shared bootloader/updater on both flash layouts, profile GPIOs,
held-pin TWAI fallback, provisioned CDC identity and 1200-baud touch. **It
has not been run.** Existing pass notes elsewhere cover earlier firmware,
not this board-profile/USB change. Do not treat a host-test or build result
as a hardware pass.

## Common setup

- **Wiring:** CANH/CANL/GND between the boards, 120 Ω at each end, optional
  brain GPIO3 → node EN: [wiring.md](../../getting-started/wiring.md).
  Bus at **250 kbit/s**.
- **CAN IDs:** older command examples assume an unprovisioned v2 block
  (`0x79E/0x79F`, empty name). The SOLO ODU preset is `0x05/0x06` with
  product `SOLO ODU Controller`, and IDU is `0x07/0x08`. Supply both
  provisioned IDs and adjust name checks when using those presets.
- **Build:** [build.md](../../getting-started/build.md). Test variants
  (`skip`, `crash`, `p5b`, `p6b`):
  [configuration.md, build variants](../../reference/configuration.md#build-variants).
- **First flash, IDU first:** [first-flash.md](../../getting-started/first-flash.md),
  including the universal bundle, provisioned identity and fallback BOOT mode.
- **Ports used below:** `COM8` = brain running the SLCAN bridge, `COM63` =
  brain running `brain_ota`, `COM7` = node; substitute yours.
- **Brain IP:** `<ip>` is the address from the brain's `IP acquired` log line.
  In PowerShell write `curl.exe`, not `curl`.

You need:

- the ODU/node, with a USB cable to native USB (GPIO19/20). While its
  factory updater runs, USB is TinyUSB CDC for identity and 1200-baud ROM
  entry, **not a logging console**. Watch updater logs on UART0; the separate
  bench app may still put its `u` input on its own USB console;
- the brain, with a USB cable to its native USB port (GPIO19/20). With the
  SLCAN bridge it is flashed once; after that this port is the SLCAN adapter;
- for the `brain_ota` tests, the brain's Ethernet on the same DHCP network
  as the PC;
- three wires between the boards: CANH, CANL and GND;
- `uv` (or Python 3.9+ with `tools/can-flash/requirements.txt` plus
  `pyserial`);
- for issue #20's GPIO/USB checklist, a 3.3 V USB-UART on the **ODU/IDU**
  UART0 (GPIO43 TX, GPIO44 RX, 115200 baud) and a scope for early GPIO3
  timing; neither board's updater sends logs over its CDC port;
- optional: a separate 3.3 V USB-UART on the brain example's UART0 to read
  SLCAN bridge bus diagnostics. The earlier CAN transfer tests do not
  require that bridge log.
