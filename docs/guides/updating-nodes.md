# Updating nodes

A node app is delivered as its OTA image, `build/<app>.bin` (e.g.
`examples/node_app/build/node_app.bin`) — never a bootloader, merged or
padded image. The node must already run the CAN updater
([first-flash.md](../getting-started/first-flash.md#the-node-the-updater)).
Three ways, from most to least automated:

| Way | PC on the CAN bus? | Parks a running app | Reference |
|---|---|---|---|
| [Update package](#1-update-package-via-the-brain) | no (Ethernet) | ENTER_UPDATER, else GPIO3 pulses | [package-format.md](../reference/package-format.md) |
| [Brain `/node/flash`](#2-one-node-via-the-brain-nodeflash) | no (Ethernet) | ENTER_UPDATER only | [brain-http-api.md](../reference/brain-http-api.md#post-nodeflash) |
| [`can_flash.py`](#3-from-a-pc-with-can_flashpy) | yes (SLCAN bridge or adapter) | `--enter-updater` / `--enter-bootloader` | [tool README](../../tools/can-flash/README.md) |

## Getting the node into its updater

How a node reaches its updater: [recovery.md](../concepts/recovery.md). In practice:

- **App built on `examples/node_app`** (CAN service): all three ways park it
  over CAN with ENTER_UPDATER; no GPIO3 pulse.
- **App without the CAN service** (e.g. `examples/bench_app`): type `u` +
  Enter on its console, or `POST /node/recover` (only the node wired to the
  brain's GPIO3), or let a package recover it automatically.
- **Already parked** (fresh board, rolled-back app, after `u`): nothing to do.

`GET /node/app_info` or `can_flash.py --app-info` tells which is running.

## 1. Update package via the brain

One file, brain image optional, up to eight nodes, power-loss-resumable.
Install the brain's partition table over USB once
([flash-layout.md](../concepts/flash-layout.md#brain-16-mb-brainpartitionscsv)).

```sh
uv run tools/pkg/ecb_pkg.py build --brain brain/build/brain_ota.bin --node examples/bench_app/build/bench_app.bin -o update.ecbpkg
uv run tools/pkg/ecb_pkg.py info update.ecbpkg
curl.exe -i --data-binary "@update.ecbpkg" http://<ip>/package
curl.exe http://<ip>/package/status
```

For a provisioned node, give its IDs and product name, e.g.
`--node "examples/bench_app/build/bench_app.bin,cmd=0x701,reply=0x702,name=axis-left"`.
Get **both IDs and the actual product name** from `/node/scan` while parked.
Repeat `--node` for up to eight entries; omit `--brain` for node-only updates.
Empty/omitted `name` disables the check. Brain-only packages are valid too.
Builder options: [tools/pkg/README.md](../../tools/pkg/README.md).

202 means staged and checked, **not finished**. Poll until `done`/`failed`,
and inspect individual node results even for `done`. The brain may reboot
first; expect a temporary network interruption and poll again after DHCP.

- A changed brain must reboot and confirm VALID before node relay; rollback
  aborts relay.
- Each node `result` names its path: `updater already parked`,
  `entered updater via CAN` or `recovered via GPIO3 pulses`. **Only the node
  physically wired to GPIO3 can be recovered by pulses**; unwired nodes must
  already be parked or run an app with the CAN service. A failed probe of an
  unwired node can reset the wired one; order the manifest accordingly.
- After a power cut mid-relay the NVS record restarts from the first node, so
  earlier successes may be reflashed.

Bench procedure: [bench/update-package.md](../testing/bench/update-package.md).

## 2. One node via the brain (`/node/flash`)

```sh
curl.exe http://<ip>/node/scan
curl.exe -i --data-binary "@examples/bench_app/build/bench_app.bin" "http://<ip>/node/flash?cmd=0x79E&reply=0x79F"
curl.exe http://<ip>/node/status
```

The brain stages the image, answers 202, sends ENTER_UPDATER (parking a
`node_app`), then flashes over CAN. Poll `/node/status` until `success` or
`failed`; **100% is not success until VERIFY and GO are ACKed.** For a
provisioned node use **both IDs from scan**. Against an app without the CAN
service ENTER_UPDATER is silent for about 1.5 s and the flash still fails
with the HELLO timeout: use `u` or `/node/recover` first.

Bench procedure: [bench/brain-can-flash.md](../testing/bench/brain-can-flash.md).

## 3. From a PC with `can_flash.py`

Needs a CAN interface on the PC: the brain running the
[SLCAN bridge](slcan-bridge.md), or any python-can adapter (CANable/slcan,
PCAN, Kvaser, gs_usb, SocketCAN, …).

```sh
# Reboot a running app into the updater, then flash its OTA image
uv run tools/can-flash/can_flash.py path/to/app/build/my_app.bin \
    --interface socketcan --channel can0 --enter-bootloader

# Park a running node app (examples/node_app) over CAN, then flash
uv run tools/can-flash/can_flash.py path/to/app/build/my_app.bin \
    --interface socketcan --channel can0 --enter-updater

uv run tools/can-flash/can_flash.py --scan      # find parked boards
uv run tools/can-flash/can_flash.py             # interactive TUI
```

With the brain's SLCAN bridge on `COM8`:

```sh
uv run tools/can-flash/can_flash.py --info --interface slcan --channel COM8
uv run tools/can-flash/can_flash.py --app-info --interface slcan --channel COM8
uv run tools/can-flash/can_flash.py examples/node_app/build/node_app.bin --enter-updater --interface slcan --channel COM8
```

A full flash run with every option:

```sh
uv run tools/can-flash/can_flash.py path/to/app/build/my_app.bin \
    --interface socketcan --channel can0 --bitrate 250000 \
    --enter-bootloader                      # if the app is running
    # --id-tx 0x700 --id-rx 0x711           # if the board has its own IDs
```

`--bitrate 250000` is can-flash's default and must match the node.
`--enter-bootloader` uses the Airform `/enter_bootloader` route over ISO-TP;
`--enter-updater` uses the native ENTER_UPDATER frame. All options:
[tools/can-flash/README.md](../../tools/can-flash/README.md#usage).
