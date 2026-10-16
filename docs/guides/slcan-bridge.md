# SLCAN bridge: the brain as a USB-CAN adapter

`examples/slcan_bridge` turns the brain (ESP32-S3-WROOM-1U-N16R8 + TCAN332DR)
into an **SLCAN (Lawicel) adapter** on its native USB-Serial-JTAG port, so
`tools/can-flash` (`can_flash.py --interface slcan --channel COMx`) reaches a
node's updater with no other CAN hardware. History: #3.

It is an alternative brain firmware, not an OTA image: flash it over USB, and
re-flash `brain_ota` ([first-flash.md](../getting-started/first-flash.md#the-brain-brain_ota))
to go back. **While brain_ota runs, the brain is not the SLCAN bridge.**

```sh
powershell -ExecutionPolicy Bypass -File build.ps1 examples/slcan_bridge -p COM8 flash   # once, over USB
uv run tools/can-flash/can_flash.py app.bin --interface slcan --channel COM8 --bitrate 250000
```

## Flash it

```sh
powershell -ExecutionPolicy Bypass -File build.ps1 examples/slcan_bridge -p COM8 flash
./build.sh examples/slcan_bridge -p /dev/ttyACM1 flash
```

`flash` writes the bridge's own bootloader, partition table and app over the
brain's USB, replacing whatever the brain held (e.g.
`build.ps1 examples/slcan_bridge -p COM63 flash`, with the brain's port). The
bridge is a standalone image, not an OTA image: built without `flash`, the
scripts print its flash offsets rather than an image to send over CAN. In a
deep Windows checkout, `flash` also builds the bridge's bootloader and CMake
prints an object-path-length warning for it; the build still completes.

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

## Check it

The node is not needed:

```sh
uv run --with python-can --with pyserial python -c "import can; b = can.Bus(interface='slcan', channel='COM8', bitrate=250000); print(b.get_version(1), b.get_serial_number(1)); b.shutdown()"
```

Expected after about 2 s (python-can waits that long after opening a port):
`(1, 1) 1A2B`, where the serial number is the last two bytes of the brain's
MAC. `(None, None) None` means `COM8` is not the bridge: wrong port, or the
brain is not running it (if a UART0 log shows `waiting for download`, the
brain sits in its ROM download mode; power-cycle it).

## Behaviour

- **Commands:** exactly python-can's `slcan` subset (`components/slcan_codec`):
  `C`, `O`, `L` (listen-only), `S0`–`S8`, `t`/`T`/`r`/`R`, `V`, `N`;
  received frames go out as `t`/`T`/`r`/`R` lines. `sXXYY`, `S9` and CAN FD
  are NAKed, and so is an `O` after `S7`: the 80 MHz TWAI clock cannot make
  750 kbit/s (so `S0`–`S8` except 750 k).
- **Bitrate.** `--bitrate 250000` (can-flash's default) makes python-can send
  the SLCAN command `S5` before opening the channel, and the bridge opens TWAI
  at 250 kbit/s, matching the node. `SLCAN_BRIDGE_BITRATE` only applies to an
  `O` without a prior `Sn` ([configuration.md](../reference/configuration.md#slcan-bridge-buildps1-examplesslcan_bridge-menuconfig--slcan-bridge)).
- **The USB port carries SLCAN only.** Logs and panics go to UART0
  (GPIO43/44, 115200): `CONFIG_ESP_CONSOLE_SECONDARY_NONE` in its
  `sdkconfig.defaults`. Bus-state changes (error-passive, bus-off and its
  automatic recovery) and lost frames are logged there, never to the SLCAN port.
- **Buffering:** a 16 KB USB RX ring (a 342-frame can-flash window is
  7.5 KB of SLCAN; the USB-Serial-JTAG driver drops host bytes on overflow
  rather than stalling USB), an 8 KB USB TX ring and a 256-frame TWAI RX
  queue.
- **GPIO3** (the node's reset, inverted: HIGH holds the node in reset) is
  driven LOW from boot so the node always runs (`components/node_reset`,
  shared with `brain/`).
- **Writes never block.** python-can's `slcan` writes never block: the bridge
  NAKs a frame it cannot queue, and python-can ignores the NAK, so can-flash
  only notices missing ACKs. can-flash's exit closes the channel (`C`), which
  stops TWAI and drops the frames still queued; the next run reopens it.
  Bus-off recovers on its own.

Bench tests through the bridge: [bench/node-updater.md](../testing/bench/node-updater.md).
