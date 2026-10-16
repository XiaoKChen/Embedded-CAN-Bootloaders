# Bench tests

Hardware procedures for the brain + node bench. **Run hardware steps only when
the bench is available.** Building/testing a change does not itself flash
boards, open serial ports, or run these tests. HTTP is trusted-LAN-only.

| Guide | Tests | Brain runs | History |
|---|---|---|---|
| [node-updater.md](node-updater.md) | the node's CAN updater from a PC: INFO/scan, flash, `u`, rollback, power loss, CAN loss | SLCAN bridge (or a USB-CAN adapter) | Phase 0, #2, #3 |
| [brain-ota.md](brain-ota.md) | the brain updating itself over Ethernet | `brain_ota` | #5 |
| [brain-can-flash.md](brain-can-flash.md) | the brain flashing a parked node over CAN (`/node/scan`, `/node/flash`) | `brain_ota` | Phase 2, #8 |
| [node-recovery.md](node-recovery.md) | crash-loop and brain-forced node recovery | `brain_ota` | Phase 3, #9 |
| [update-package.md](update-package.md) | brain-first update package and resumable node relay | `brain_ota` | Phase 5, #10 |
| [node-app.md](node-app.md) | the reference node app, parked over CAN without GPIO3 | `brain_ota` | Phase 6, #11 |

## Common setup

- **Wiring:** CANH/CANL/GND between the boards, 120 Ω at each end, optional
  brain GPIO3 → node EN: [wiring.md](../../getting-started/wiring.md).
  Bus at **250 kbit/s**.
- **Build:** [build.md](../../getting-started/build.md). Test variants
  (`skip`, `crash`, `p5b`, `p6b`):
  [configuration.md, build variants](../../reference/configuration.md#build-variants).
- **First flash, brain first:** [first-flash.md](../../getting-started/first-flash.md),
  including identifying the COM ports, BOOT-button download mode and the
  serial monitor.
- **Ports used below:** `COM8` = brain running the SLCAN bridge, `COM63` =
  brain running `brain_ota`, `COM7` = node; substitute yours.
- **Brain IP:** `<ip>` is the address from the brain's `IP acquired` log line.
  In PowerShell write `curl.exe`, not `curl`.

You need:

- the node, with a USB cable to its USB-Serial-JTAG port (GPIO19/20). The
  node keeps this connection through the tests: it carries the first
  updater flash and the node's console (logs, and the bench app's `u`
  command);
- the brain, with a USB cable to its native USB port (GPIO19/20). With the
  SLCAN bridge it is flashed once; after that this port is the SLCAN adapter;
- for the `brain_ota` tests, the brain's Ethernet on the same DHCP network
  as the PC;
- three wires between the boards: CANH, CANL and GND;
- `uv` (or Python 3.9+ with `tools/can-flash/requirements.txt` plus
  `pyserial`);
- optional: a 3.3 V USB-UART on the brain's UART0 (GPIO43 TX, GPIO44 RX,
  115200 baud) to read the bridge's log (bus errors, frame loss) or early
  bootloader lines. The tests do not need it.
