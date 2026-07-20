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

- [x] **DAC12** (`dac.rs`) — `Ra4m1Dac.cs` @ 0x4005_E000 (DADR0 read-back + DACR.DAOE0);
      `analog.robot` async ADC→DAC case. Static-checked (Renode not installed locally).
- [x] **UART/SCI** (`sci.rs`) — stock `RenesasRA6M5_SCI` layout confirmed against the driver;
      `serial.robot` echo case. **DIVERGENCE:** stock repl wires RXI/TXI/TEI to 0xA3/0xA4/0xA5 but
      `sci.rs` programs 0x97/0x98/0x99 — both unverified FSP orderings, BENCH-VERIFY the real numbers.
- [x] **I2C/RIIC** (`i2c.rs`) — stock `RenesasRA_IIC` + repl IRQ wiring (0x035..0x038) both match
      the driver; `MockI2CSlave.cs` register-file slave @ 0x50; `serial.robot` scan + read-back cases.
      Read-back case needs an `examples/i2c_regread` ELF (a `write_read` example) that doesn't exist yet.
- [x] **ADC14** (`adc.rs`) — `Ra4m1Adc.cs` @ 0x4005_C000 (ADCSR.ADST self-clear, ADDR[0..14],
      S12ADI0 → icu@0x29); `analog.robot` blocking + async cases.
- [x] **SPI** (`spi.rs`) — `Ra4m1Spi.cs` full loopback model (SPCR/SPSR/SPDR, SPTI/SPRI IRQs);
      `serial.robot` loopback case.

Note: Renode validates driver **register-logic**, not chip-facts. The drivers' BENCH-VERIFY items
(ICU event numbers, pin/PSEL maps) must still be confirmed on silicon — a Renode model coded from
the same assumptions would pass circularly.

## Bootloader coverage (validates M0–M2)

- [x] **FACI-LP flash P/E** — `Ra4m1FlashLp.cs` @ 0x407E_C000 (FENTRYR mode, FSTATR1.FRDY, FCR
      ERASE 0x84 / PROGRAM 0x81 into backing code-flash); `bootloader.robot` erase→program→read-back
      case. Models no P/E timing or fetch-stall hazard (bench-only).
- [x] **CAN0** — `Ra4m1Can0.cs` @ 0x4005_0000 bridged to stock `CANHub`; `bootloader.robot` HELLO→
      reply case via native `CANTester`. Frame-level, not bit-level. The `can_flash.py` end-to-end
      loop still needs `CreateSocketCANBridge` (Linux-only) — documented in `bootloader.resc`.
- [x] **VBTBKR survival across reset** — `MappedMemory @ 0x4001_E500` (retained across `cpu Reset`);
      `bootloader.robot` asserts magic survives reset and the boot decision consumes+clears it.

## Out of scope

- **USB DFU / USBFS** — Renode USB is export-only; there is no RA4M1 USBFS **device** model.
  USB DFU validation stays on the HIL bench (`dfu-util`).
- **Timing / clock tolerance, flash P/E fetch-stall, analog accuracy, CAN bit-timing** — Renode is
  functional, not cycle-accurate. Bench-only.

## Infra

- [x] Wire the suites into CI (`.github/workflows/ci.yml`) — new `renode` job, `needs: firmware-build`,
      pinned `antmicro/renode:1.16.1`, runs `renode/*.robot` from the repo root and uploads results.
      Rebuilds the ra4m1-hal example + bootloader ELFs the suites `LoadELF` (firmware-build doesn't emit them).
- [x] As models land, add each new `.robot` case + C# model under `renode/` — `analog`, `serial`,
      `bootloader` suites + `Ra4m1Dac/Adc/Spi/FlashLp/Can0.cs` + `MockI2CSlave.cs` added.

> **All 12 cases across the 4 suites pass on Renode v1.16.1**, run locally (portable install) and in
> the new CI `renode` job. Every model mirrors driver/PAC assumptions, so ICU event numbers and
> pin/PSEL maps still need silicon BENCH-VERIFY (the circularity caveat above) — a green run here
> validates register/protocol logic, not the chip-facts the drivers were coded against.
