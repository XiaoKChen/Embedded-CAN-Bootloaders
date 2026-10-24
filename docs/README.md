# Documentation

Docs for the ESP32-S3 common factory CAN updater, SOLO ODU/IDU layouts and
the separate brain example. Start with [getting-started/build.md](getting-started/build.md), then
[first-flash.md](getting-started/first-flash.md).

## getting-started/

| Doc | Purpose |
|---|---|
| [build.md](getting-started/build.md) | one-command build, prerequisites, outputs, existing ESP-IDF / Docker, paths with spaces, CI and releases |
| [first-flash.md](getting-started/first-flash.md) | universal bundle, SOLO board presets, provisioned USB identity and ROM download |
| [wiring.md](getting-started/wiring.md) | transceiver, profile-driven pins, termination, IDU GPIO3 → ODU EN |

## concepts/

| Doc | Purpose |
|---|---|
| [system-overview.md](concepts/system-overview.md) | how brain, node updater, node apps, packages and tools fit together; update flows |
| [flash-layout.md](concepts/flash-layout.md) | 4 MB ODU, 16 MB IDU and separate brain example tables; 7.5 MiB package cap |
| [recovery.md](concepts/recovery.md) | entry paths, counters, provisioned holds, updater USB and 1200-baud ROM entry |

## reference/

| Doc | Purpose |
|---|---|
| [can-protocol.md](reference/can-protocol.md) | CAN Update Protocol v1 frames, APP_INFO / ENTER_UPDATER, app vs updater vs silence, host limits |
| [brain-http-api.md](reference/brain-http-api.md) | every brain HTTP endpoint: parameters, status codes, JSON, state names, status texts |
| [package-format.md](reference/package-format.md) | `.ecbpkg` v1 wire format, brain-first transaction, NVS record, power-loss table |
| [configuration.md](reference/configuration.md) | updater profile/defaults vs. remaining app/bridge Kconfig settings |
| [board-profile.md](reference/board-profile.md) | v1/v2 record contract, GPIO and I2C rules, safe writes, defaults, golden vectors, SOLO presets |

## guides/

| Doc | Purpose |
|---|---|
| [updating-nodes.md](guides/updating-nodes.md) | flash nodes via package, brain `/node/flash`, or `can_flash.py` |
| [writing-a-node-app.md](guides/writing-a-node-app.md) | reference ESP-IDF app and stock Arduino helper option |
| [platformio-arduino.md](guides/platformio-arduino.md) | opt-in stock Arduino node-app helper, plus custom-RTC client path |
| [provisioning-can-ids.md](guides/provisioning-can-ids.md) | ESP-PROG-2-PY-CLI presets and manual v2/profile recipe |
| [slcan-bridge.md](guides/slcan-bridge.md) | the brain as an SLCAN USB-CAN adapter for PC tools |

## testing/

| Doc | Purpose |
|---|---|
| [host-tests.md](testing/host-tests.md) | Unity/CTest suites on the PC, what they cover |
| [bench/README.md](testing/bench/README.md) | bench test index and common setup |
| [bench/node-updater.md](testing/bench/node-updater.md) | existing CAN procedure; **not-yet-run** issue #20 profile/USB checklist |
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
