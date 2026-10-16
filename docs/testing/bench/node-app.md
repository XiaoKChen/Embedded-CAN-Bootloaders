# Bench: reference node app

Tests `examples/node_app`, the reference app the brain parks over CAN with
ENTER_UPDATER, without GPIO3. **Run these hardware steps only when the bench
is available.** Building this change does not flash boards or open serial
ports. HTTP stays trusted-LAN-only. What the app does:
[writing-a-node-app.md](../../guides/writing-a-node-app.md); the two
commands: [can-protocol.md](../../reference/can-protocol.md#app-control-app_info-and-enter_updater).

The node updater and bootloader are **unchanged**: nodes with the recovery
install ([node-recovery.md](node-recovery.md#install-once-over-usb)) need no
USB reflash. The brain needs firmware with ENTER_UPDATER support (Phase 6):
the [update-package.md](update-package.md#one-time-partition-table-installation-and-package-build)
USB flow, or `/ota` when its table is already current.

History: Phase 6, issue #11. Common setup: [README.md](README.md).

## Build

```sh
powershell -ExecutionPolicy Bypass -File build.ps1
powershell -ExecutionPolicy Bypass -File build.ps1 examples/node_app
powershell -ExecutionPolicy Bypass -File build.ps1 examples/bench_app
./build.sh examples/node_app          # Linux / macOS
```

The OTA image is `examples/node_app/build/node_app.bin`. Like `bench_app`, never
`idf.py flash` it onto a node: it would replace the updater in `factory`. A
second, distinguishable version for the
[CAN-path test](#second-package-while-node_app-runs-can-path-no-gpio3-pulse)
(separate build directory):

```sh
powershell -ExecutionPolicy Bypass -File build.ps1 examples/node_app -B p6b -D SDKCONFIG=p6b/sdkconfig -D PROJECT_VER=p6-next app
```

## First package while bench_app runs: GPIO3 path expected

Start with `bench_app` running on the node for more than 10 s
([node-updater.md (b)](node-updater.md#b-flash-the-bench-app-over-can-go-app-confirms-itself)
or a package). Upload a node-only package with the node app:

```sh
uv run tools/pkg/ecb_pkg.py build --node examples/node_app/build/node_app.bin -o node_app.ecbpkg
curl.exe -i --data-binary "@node_app.ecbpkg" http://<ip>/package
curl.exe http://<ip>/package/status
```

`bench_app` has no CAN service, so expect `probing` with `Sending
ENTER_UPDATER over CAN` (about 1.5 s of silence), then `No CAN entry; pulsing
GPIO3 ...`, `flashing`, and finally node
`state:ok`, `result:"ok; recovered via GPIO3 pulses"`. The node console then
shows the node app:

```
I (...) node_app: node_app version <git describe> (ESP-IDF v5.3.6), running from ota_0, PENDING_VERIFY
I (...) can_service: listening: cmd ID 0x79E, reply ID 0x79F (defaults), TWAI TX GPIO 4 / RX GPIO 5, 250000 bit/s
I (...) node_app: self-test passed; image marked valid (rollback cancelled)
I (...) node_app: application running (placeholder: replace app_run() in node_app.c)
```

## Second package while node_app runs: CAN path, no GPIO3 pulse

With the node app from the previous test running, upload the second version:

```sh
uv run tools/pkg/ecb_pkg.py build --node examples/node_app/p6b/node_app.bin -o node_app_next.ecbpkg
curl.exe -i --data-binary "@node_app_next.ecbpkg" http://<ip>/package
curl.exe http://<ip>/package/status
```

Expect `probing` with `Sending ENTER_UPDATER over CAN` (the brain sends it
first, before any INFO), then `INFO probe` once the updater has booted, and on
the node:

```
W (...) can_service: ENTER_UPDATER: acknowledging, then restarting into the CAN updater
...
I (...) can_updater: parked: cmd ID 0x79E, reply ID 0x79F, 250000 bit/s, slot ota_0 (1835008 bytes)
```

The brain waits 1.5 s, INFO answers, the transfer runs, and the node result is
`"ok; entered updater via CAN"`. **No GPIO3 pulse may occur**: GPIO3 stays LOW
(scope/LED), the status never shows `pulsing GPIO3`, and the brain log has
`ENTER_UPDATER cmd 0x79E reply 0x79F: ENTER_UPDATER: success (attempts 1)`.
The new image boots, confirms itself, and APP_INFO reports `p6-next`. Repeat
this package a few times back to back: the ENTER restart is clean, so
recovery must never trip.

A provisioned node works the same with its own pair: package it with
`--node "...node_app.bin,cmd=0x701,reply=0x702,name=<product>"`; the node app
logs `(per-device, cancfg)` and answers only on `0x701`.

## APP_INFO and `/node/flash`

```sh
curl.exe "http://<ip>/node/app_info?cmd=0x79E&reply=0x79F"
```

Expected per node state (node app running → `mode:"app"` with `p6-next`;
parked → `mode:"updater"`; bench_app or no node → `504` after about 1.5 s):
[`GET /node/app_info`](../../reference/brain-http-api.md#get-nodeapp_info).
The route shares the update worker: 409 while a scan, flash or package runs.
The APP_INFO probe of a parked updater leaves its session untouched; a
following `/node/flash` must still succeed.

`POST /node/flash` also parks a running node app: it sends ENTER_UPDATER before
HELLO and, on an app ACK, waits 1.5 s for the updater. The final `/node/status`
text ends with `(entered updater via CAN)`. A parked updater NAKs ENTER_UPDATER
at once; against `bench_app` it is silent for about 1.5 s and the flash still
fails with the HELLO timeout (use `u` or `/node/recover`).

From a PC, with the brain running the SLCAN bridge
([slcan-bridge.md](../../guides/slcan-bridge.md)) or another adapter
([node-updater.md](node-updater.md#alternative-a-usb-can-adapter-instead-of-the-brain)):

```sh
uv run tools/can-flash/can_flash.py --app-info --interface slcan --channel COM8
uv run tools/can-flash/can_flash.py examples/node_app/build/node_app.bin --enter-updater --interface slcan --channel COM8
```

## bench_app is unchanged

Flash `bench_app` again (package or
[node-updater.md (b)](node-updater.md#b-flash-the-bench-app-over-can-go-app-confirms-itself))
and repeat [(c)](node-updater.md#c-u-on-the-console-brings-the-updater-back):
`u` + Enter still enters the updater. `bench_app` sends no CAN traffic and
ignores APP_INFO/ENTER_UPDATER, which is why the
[first package](#first-package-while-bench_app-runs-gpio3-path-expected)
takes the GPIO3 path.
