# Hardware bench-verify TODO

Items that only **silicon on the HIL bench** (UNO R4 Minima + SWD probe) can confirm — surfaced
by the M2.5 review (2026-07-19) plus the standing "functional-not-cycle-accurate" boundary of the
Renode harness. A green Renode run validates register/protocol **logic** against the drivers' own
assumptions; it cannot validate the chip-facts those assumptions encode (that would be circular).
See [`../../renode/TODO.md`](../../renode/TODO.md) for what emulation *does* cover.

Legend: **P1** = blocks trusting the feature on hardware · **P2** = correctness corner · **P3** = polish.

## ICU event-number verification (P1 — highest hardware risk)

The async HAL peripherals route interrupts by writing an assumed FSP event number straight into
`ICU.IELSR[slot].IELS`. Every number below is a **contiguous-ordering guess** anchored off the two
silicon-verified events (USBFS `0x033`, GPT0 CCMPA `0x057`) — none of the peripheral bases are pinned.
A wrong number routes a dead event: the async future registers its waker and **never wakes** (silent
hang), while the blocking busy-poll paths keep working and mask it in smoke tests.

- [ ] **SCI2 RXI/TXI/TEI** — code says `0x97/0x98/0x99` (`crates/ra4m1-hal/src/sci.rs:406-445`); the stock
      Renode model wires SCI2 to `0xA3/0xA4/0xA5` — a 12-event divergence. SCI2 is the UNO R4's
      `Serial1` (D0/D1), the most-used UART, so this is the top item. Confirm against R01UM0007's
      ICU event table (or FSP `bsp_elc.h`) and correct the `impl_instance!` rows if wrong.
- [ ] **SPI0 SPTI/SPRI** — `0xA1/0xA2` (`spi.rs:355-356`).
- [ ] **IIC RXI/TXI/TEI/EEI** — `0x035..0x03D` (`i2c.rs:352-373`).
- [ ] **ADC S12ADI0** — `0x029` (`adc.rs:74`).

> Partial hardware-free path: the custom Renode GPT model could be extended to deliver an SCI/SPI
> event and prove the waker chain wakes — but the *number itself* still needs the silicon RM/FSP
> cross-check, since a Renode model coded from the same guess would agree circularly.

## Peripheral register-timing corners (P2)

- [ ] **RIIC single-byte master receive** (`i2c.rs:717-745`) — for a 1-byte read, `ICMR3.ACKBT` (last-byte
      NACK) is armed *after* the dummy `ICDRR` read that clocks the byte in. Scope SCL/SDA on the bench;
      if the STOP framing is corrupted, set `ACKBT` **before** the dummy read when `total == 1`.
- [ ] **RIIC zero-length read run** (`i2c.rs:717-745`) — a merged read run whose buffers are all empty
      (`total == 0`) starts a reception but never arms the terminating NACK before `stop()`. Confirm the
      `embedded-hal` zero-length case behaves, or short-circuit it.
- [ ] **DAC12 reference/output settle** (`dac.rs:94-99`) — no wait between `DAVREFCR.REF=001` and
      `DACR.DAOE0=1`; R01UM0007 specifies a reference-buffer settle + tDACR. First `DADR0` output may be
      invalid until it settles. Measure settle time; decide whether `new()` should include a few-µs wait
      (currently punted to the caller via `Timer::after`).

## Clock / operating-mode precondition (P2)

- [ ] **OPCCR.OPCM = 0 for flash P/E** — `flash_lp.rs:40-43` states high-speed operating mode is a
      precondition "owned by `clocks::init_hoco_48mhz`", but `init_hoco_48mhz` never writes `OPCCR`
      (`crates/ra4m1-embassy/src/clocks.rs`). Correctness holds today only by RA4M1 reset default. On the
      bench, confirm `OPCM=0` at flash-P/E time; then either write it explicitly in `init_hoco_48mhz` or
      fix the header to say it relies on the reset default. (Becomes P1 if any low-power mode is ever added.)

## Standing bench-only boundary (P1/P2 — Renode structurally cannot cover)

Recorded from the frozen SPEC + `renode/TODO.md` "Out of scope"; these were always HIL-bench exit
criteria, not regressions:

- [ ] **Flash P/E timing + RAM-residency fetch-stall hazard** — the FACI leaves run from RAM with
      interrupts masked; Renode completes P/E instantly, so a RAM-residency regression that would brick
      silicon passes green in emulation. Verify erase/program timing and that no code-flash fetch occurs
      mid-sequence.
- [ ] **CAN0 bit-timing** — BRP / sample point / SJW / arbitration / bus-off recovery at 250 kbit/s
      against a real transceiver + `can_flash.py` end-to-end (the SocketCAN bridge path deferred out of CI).
- [ ] **USB DFU / USBFS device** — no RA4M1 USBFS device model exists in Renode; full `dfu-util`
      enumerate → download → manifest → reboot stays bench-only (see [[usb-dfu-bringup-debug]]:
      root-caused to UCLK config on first silicon).
- [ ] **Real UART baud / framing** — actual bit rate, start/stop/parity, rise/fall at the pin.
- [ ] **HOCO / clock tolerance** — 48 MHz accuracy and wall-clock timing margin (Renode time is virtual).
- [ ] **Analog accuracy** — ADC14 conversion/settling and DAC12 output voltage vs reference.

## Testability follow-up (P3, not hardware)

- [ ] Hoist the DFU `machine` state machine out of `bootloader/src/tasks/usb_dfu.rs` into
      `crates/boot-logic` (it has zero USB/HAL deps) so `cargo test` gives it standing host coverage —
      today the `bootloader` bin is `test = false`, so its tests never compile in-tree.
