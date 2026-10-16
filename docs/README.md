# Documentation

Docs for the ESP32-S3 brain + node CAN updater on branch `version/c-cpp/esp32`.
Start with [getting-started/build.md](getting-started/build.md), then
[first-flash.md](getting-started/first-flash.md).

## getting-started/

| Doc | Purpose |
|---|---|
| [build.md](getting-started/build.md) | one-command build, prerequisites, outputs, existing ESP-IDF / Docker, paths with spaces, CI and releases |
| [first-flash.md](getting-started/first-flash.md) | first USB flash of brain then node, COM ports, BOOT-button download mode, monitor |
| [wiring.md](getting-started/wiring.md) | transceivers, pin tables, termination, the brain GPIO3 → node EN reset line |

## concepts/

| Doc | Purpose |
|---|---|
| [system-overview.md](concepts/system-overview.md) | how brain, node updater, node apps, packages and tools fit together; update flows |
| [flash-layout.md](concepts/flash-layout.md) | node and brain partition tables, package size limits |
| [recovery.md](concepts/recovery.md) | every path into the node's updater, recovery counters, stable clear, fail-safe and power loss |

## reference/

| Doc | Purpose |
|---|---|
| [can-protocol.md](reference/can-protocol.md) | CAN Update Protocol v1 frames, APP_INFO / ENTER_UPDATER, app vs updater vs silence, host limits |
| [brain-http-api.md](reference/brain-http-api.md) | every brain HTTP endpoint: parameters, status codes, JSON, state names, status texts |
| [package-format.md](reference/package-format.md) | `.ecbpkg` v1 wire format, brain-first transaction, NVS record, power-loss table |
| [configuration.md](reference/configuration.md) | menuconfig/Kconfig options, `sdkconfig.defaults` notes, build variants |

## guides/

| Doc | Purpose |
|---|---|
| [updating-nodes.md](guides/updating-nodes.md) | flash nodes via package, brain `/node/flash`, or `can_flash.py` |
| [writing-a-node-app.md](guides/writing-a-node-app.md) | start from `examples/node_app`, integrate `can_updater_client`, recovery contract |
| [provisioning-can-ids.md](guides/provisioning-can-ids.md) | per-device CAN IDs and INFO name in `cancfg` |
| [slcan-bridge.md](guides/slcan-bridge.md) | the brain as an SLCAN USB-CAN adapter for PC tools |

## testing/

| Doc | Purpose |
|---|---|
| [host-tests.md](testing/host-tests.md) | Unity/CTest suites on the PC, what they cover |
| [bench/README.md](testing/bench/README.md) | bench test index and common setup |
| [bench/node-updater.md](testing/bench/node-updater.md) | node updater from a PC via the SLCAN bridge or an adapter |
| [bench/brain-ota.md](testing/bench/brain-ota.md) | brain self-update over Ethernet |
| [bench/brain-can-flash.md](testing/bench/brain-can-flash.md) | brain flashes a parked node over CAN |
| [bench/node-recovery.md](testing/bench/node-recovery.md) | crash-loop and brain-forced recovery |
| [bench/update-package.md](testing/bench/update-package.md) | update package and resumable relay |
| [bench/node-app.md](testing/bench/node-app.md) | reference node app, CAN entry without GPIO3 |

## Tool READMEs

Kept next to their tools:

- [tools/can-flash/README.md](../tools/can-flash/README.md) — python-can host
  flasher / scanner / TUI, all options, `--selftest`.
- [tools/pkg/README.md](../tools/pkg/README.md) — package builder and
  integrity inspector, node option syntax, parser test vector.

## Other branches

The Arduino UNO R4 Minima (RA4M1) research document,
`docs/research/r4-minima-can-bootloader.md`, is RA4M1-only and lives on branch
`version/c-cpp/ra4m1` at that path.
