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
TX GPIO (profile) ───────► TXD                 CANH ───────
RX GPIO (profile) ◄─────── RXD                 CANL ───────
3V3 ──────────────────── VCC (and VIO)                 120 Ω at both ends
GND ──────────────────── GND ─── common ground with the host adapter
```

TX and RX are per-board values from the [board profile](../reference/board-profile.md)
written to `cancfg`, not updater menuconfig settings. The defaults (when no
valid profile exists) are TX4/RX5 at **250 kbit/s** with no hold pins; they
match the ODU, not the IDU. The application must also configure its own TWAI
driver for the physical wiring.

## Pins

| Board | Function | GPIO | Notes |
|---|---|---|---|
| ODU | TWAI TX → TCAN332 `D` | GPIO4 | ODU profile (fallback also TX4); reference app has separate `NODE_APP_TWAI_TX_GPIO` |
| ODU | TWAI RX ← TCAN332 `R` | GPIO5 | ODU profile (fallback also RX5); reference app has separate `NODE_APP_TWAI_RX_GPIO` |
| ODU | native USB | GPIO19/20 | updater TinyUSB CDC identity; ROM download or app USB behaviour differs |
| ODU | EN | — | from IDU GPIO3 (inverted), below |
| IDU | TWAI TX → TCAN332 `D` | GPIO6 | IDU profile; brain app and SLCAN bridge configure this separately |
| IDU | TWAI RX ← TCAN332 `R` | GPIO7 | IDU profile; brain app and SLCAN bridge configure this separately |
| IDU | node reset (EN, inverted) | GPIO3 | IDU profile holds it LOW from 2nd-stage boot onward; brain app may pulse it |
| IDU | W5500 on SPI2 (IOMUX) | MOSI GPIO11, MISO GPIO13, SCLK GPIO12, CS GPIO10, INTn GPIO9, RSTn GPIO14 | SPI at 20 MHz |
| IDU | native USB | GPIO19/20 | updater CDC; ROM download or SLCAN bridge has different identity |
| IDU | UART0 | GPIO43 TX, GPIO44 RX | 115200 baud updater console |

**No GPIO factory-reset pin:** the ESP-IDF factory-reset Kconfig feature is
off. Recovery uses counters and CAN entry
([recovery.md](../concepts/recovery.md)).

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
- **A floating GPIO3 can hold the ODU in reset.** The IDU's valid profile
  drives GPIO3 LOW after `bootloader_init()`, before the partition table is
  logged, and the updater keeps the hold. ROM download and a blank IDU run
  earlier than that; GPIO3 may still float and prevent ODU USB enumeration.
  Flash the IDU first, then plug in or reset the ODU.
- **IDU reboots may reset the ODU.** GPIO3 can float during ROM boot even with
  a valid profile. From 2nd-stage initialization onward the hold is LOW;
  `brain_ota` also drives GPIO3 LOW in `app_main`. The brief ROM interval can
  hold EN through the inverting stage and restart the ODU. This is accepted
  on the bench; a connected ODU may re-enumerate.
- **GPIO3 stays LOW** except for `/node/recover` or a package's automatic
  recovery (four 100 ms HIGH / 400 ms LOW pulses through the inverting
  stage). Only the physically wired node can be recovered.
- **GPIO3 is an ESP32-S3 strapping pin, but only if the `STRAP_JTAG_SEL`
  eFuse is burned** (it selects the JTAG source at reset). On a default chip
  it is not burned, so the level on GPIO3 at the brain's own reset does not
  matter. Leave that eFuse alone.
