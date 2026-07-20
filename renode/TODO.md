# Renode coverage TODO

Roadmap for growing the M2.5 Renode harness from the current **minimal smoke** to
per-peripheral coverage. Effort ratings and stock-platform status come from the M2.5
de-risk spike (see [`README.md`](README.md) and `docs/plan-m2.5-renode.md`).

Legend: **S/M/L** = small/medium/large C# effort · "stock" = modeled by Renode
v1.16.1's `renesas-r7fa4m1a.repl` out of the box · "Tag" = unimplemented stub.

## Done (minimal smoke)

- [x] **GPT320 time base** — custom `Ra4m1Gpt320.cs` (GTCR.CST start + GTCCRA compare-match on a
      free-running counter → CCMPA → ICU 0x57 → IEL0). The piece that makes the executor tick.
- [x] **GPIO output** — P111/D13 LED binding (PORT is stock); `blinky_gpio` asserts 1 Hz blink.
- [x] **Output observation** — LED tester + defmt-rtt WrOff growth; `irq_smoke` heartbeat.
- [x] **Robot suite** `smoke.robot` (green; negative control fails on stock GPT).

## HAL peripheral coverage (validates M3 `ra4m1-hal` drivers)

- [ ] **DAC12** (`dac.rs`) — **S**. Tag today. Model `DADR` write + `DACR.DAOE0`. Test: set a
      12-bit value, assert `DADR`/output register reads back.
- [ ] **UART/SCI** (`sci.rs`) — **S**. `RenesasRA6M5_SCI` is stock; confirm the RA4M1 register
      variant matches. Test: attach a UART backend, TX a string, `Wait For Line On Uart`; loop RX.
- [ ] **I2C/RIIC** (`i2c.rs`) — **S**. `RenesasRA_IIC` is stock; add a mock I2C **slave device** on
      the bus. Test: `write_read` a register, assert the transaction (START/addr/data/STOP).
- [ ] **ADC14** (`adc.rs`) — **M**. Tag today. Model `ADCSR` scan (ADST self-clear), `ADDR[n]`
      results, and the S12ADI scan-complete IRQ. Test: blocking `read()` + async `read_async()`.
- [ ] **SPI** (`spi.rs`) — **M–L**. Tag today. Full model: `SPCR/SPSR/SPDR`, TX/RX IRQs, mode/bit
      order. Test: loopback `transfer_in_place`, assert the echoed pattern.

Note: Renode validates driver **register-logic**, not chip-facts. The drivers' BENCH-VERIFY items
(ICU event numbers, pin/PSEL maps) must still be confirmed on silicon — a Renode model coded from
the same assumptions would pass circularly.

## Bootloader coverage (validates M0–M2)

- [ ] **FACI-LP flash P/E** — **M**. Highest-value next increment: unlocks the S3 erase→program→
      read-back loop and the boot-decision/`boot5` integration paths host tests can't reach.
- [ ] **CAN0** — **M–L**. Model CAN0 + a SocketCAN/CAN-hub bridge so an unmodified `can_flash.py`
      can drive the emulated bootloader end-to-end (plan `docs/plan-m2.5-renode.md` M2.5.3).
- [ ] **VBTBKR survival across reset** — **S**. Assert the boot-mode handoff words persist across
      `sysbus.cpu Reset` (double-tap / CAN-entry magics).

## Out of scope

- **USB DFU / USBFS** — Renode USB is export-only; there is no RA4M1 USBFS **device** model.
  USB DFU validation stays on the HIL bench (`dfu-util`).
- **Timing / clock tolerance, flash P/E fetch-stall, analog accuracy, CAN bit-timing** — Renode is
  functional, not cycle-accurate. Bench-only.

## Infra

- [ ] Wire `smoke.robot` into CI (`.github/workflows/ci.yml`) via `antmicro/renode-test-action` or
      the pinned `antmicro/renode:1.16.1` image, gated behind `firmware-build`.
- [ ] As models land, add each new `.robot` case + any C# model file under `renode/`.
