# Wiring

## Boards

- **Brain:** ESP32-S3-WROOM-1U-N16R8 (16 MB flash, 8 MB octal PSRAM) with a
  TCAN332DR and a W5500 Ethernet controller.
- **Node:** ESP32-S3FH4R2 (4 MB in-package flash, 2 MB PSRAM) with a
  TCAN332DR (issue #2).

## CAN transceiver

The ESP32-S3's TWAI controller drives logic-level pins only and goes through
the GPIO matrix to an external **3.3 V transceiver**. Use a 3.3 V part
(SN65HVD230, TCAN332, MCP2562 with VIO = 3.3 V), so its R output feeds the S3
directly. Tie any standby/silent pin to its active level.

```
ESP32-S3                 3.3 V CAN transceiver           bus
TX GPIO (4) ───────────► TXD                 CANH ───────
RX GPIO (5) ◄─────────── RXD                 CANL ───────
3V3 ──────────────────── VCC (and VIO)                 120 Ω at both ends
GND ──────────────────── GND ─── common ground with the host adapter
```

TX GPIO, RX GPIO and bitrate are menuconfig options
([configuration.md](../reference/configuration.md)); the defaults match the
boards below at **250 kbit/s**.

## Pins

| Board | Function | GPIO | Notes |
|---|---|---|---|
| node | TWAI TX → TCAN332 `D` | GPIO4 | `CAN_UPDATER_TWAI_TX_GPIO`, `NODE_APP_TWAI_TX_GPIO` |
| node | TWAI RX ← TCAN332 `R` | GPIO5 | `CAN_UPDATER_TWAI_RX_GPIO`, `NODE_APP_TWAI_RX_GPIO` |
| node | USB-Serial-JTAG | GPIO19/20 | first flash, console (logs, the bench app's `u`) |
| node | EN | — | from the brain's GPIO3 (inverted), below |
| brain | TWAI TX → TCAN332 `D` | GPIO6 | C CAN host (`brain/`) and `SLCAN_BRIDGE_TWAI_TX_GPIO` |
| brain | TWAI RX ← TCAN332 `R` | GPIO7 | C CAN host and `SLCAN_BRIDGE_TWAI_RX_GPIO` |
| brain | node reset (EN, inverted) | GPIO3 | `NODE_RESET_GPIO` (`components/node_reset`) |
| brain | W5500 on SPI2 (IOMUX) | MOSI GPIO11, MISO GPIO13, SCLK GPIO12, CS GPIO10, INTn GPIO9, RSTn GPIO14 | SPI at 20 MHz |
| brain | USB-Serial-JTAG (native USB) | GPIO19/20 | first flash, console; SLCAN port when running the bridge |
| brain | UART0 | GPIO43 TX, GPIO44 RX | 115200 baud console |

**No recovery pin:** GPIO4 is TWAI TX, so the bootloader's GPIO factory
reset is off ([recovery.md](../concepts/recovery.md)).

## Brain ↔ node

Each board carries its own TCAN332DR. Between the boards run only the bus,
ground and (optionally) the reset line:

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

A PC USB-CAN adapter wires the same way in the brain's place; put the second
120 Ω at the adapter (most have a jumper or switch for it). Do not feed a
5 V-logic transceiver's RXD straight into an ESP32-S3 pin.

## Brain GPIO3 → node EN (reset line)

The brain controls the node's EN (reset) from its **GPIO3**. That is the
only brain-to-node wire besides CAN and ground. Recovery uses four
deliberate reset pulses ([recovery.md](../concepts/recovery.md)).

- **Polarity is inverted: GPIO3 HIGH = node held in reset, LOW = node runs.**
  Firmware on the brain (`brain_ota` and the SLCAN bridge, through
  `components/node_reset`) drives GPIO3 push-pull LOW as early as possible
  (latching the level before enabling the output, so it never glitches high)
  and pulses it HIGH only to reset the node.
- **A floating GPIO3 can hold the node in reset.** While the brain is blank,
  in ROM download mode, being flashed or still booting, GPIO3 is not driven;
  on these boards that has been observed to keep the node in reset (no USB
  enumeration). Flash the brain first, then plug in or reset the node.
- **Brain reboots reset the node.** brain_ota drives GPIO3 LOW first thing in
  `app_main`. From each brain reset until then (the bootloader plus start-up,
  well under a second) GPIO3 floats, so **every OTA reboot, rollback and reset
  of the brain can hold the node in reset briefly, and the node restarts.**
  That is accepted for this bench. A node connected over USB re-enumerates.
- **GPIO3 stays LOW** except for `/node/recover` or a package's automatic
  recovery (four 100 ms HIGH / 400 ms LOW pulses through the inverting
  stage). Only the physically wired node can be recovered.
- **GPIO3 is an ESP32-S3 strapping pin, but only if the `STRAP_JTAG_SEL`
  eFuse is burned** (it selects the JTAG source at reset). On a default chip
  it is not burned, so the level on GPIO3 at the brain's own reset does not
  matter. Leave that eFuse alone.
