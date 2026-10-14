# Renode emulation harness (M2.5)

Boots the **real** `ra4m1-hal` example ELFs and the bootloader ELF in the
[Renode](https://github.com/renode/renode) emulator (MIT-licensed, a test-time tool only — never
linked into firmware) and proves the async executor, the peripheral drivers, and the boot/flash/CAN
protocols run against register-level models — headless and deterministically, without hardware.

This is the **full per-peripheral coverage** of `docs/plan-m2.5-renode.md`: **12 cases across 4
Robot suites** (`smoke`, `analog`, `serial`, `bootloader`), all green on Renode **v1.16.1**.
Coverage: GPT time base + GPIO (smoke), DAC12 + ADC14 (analog), SPI + I2C/RIIC + UART/SCI (serial),
and FACI-LP flash P/E + CAN0 + VBTBKR-across-reset (bootloader). USB DFU stays out of scope (Renode
USB is export-only; there is no RA4M1 USBFS device model).

## Files

| File | Role |
|------|------|
| `ra4m1-uno-r4.repl` | Platform: `using` the stock `renesas-r7fa4m1a.repl`, cancels the stock `gpt`, binds the custom models at their real addresses, adds the D13/P111 LED. |
| `Ra4m1Gpt320.cs` | Custom GPT320 channel-0 model — the one missing piece. Closes three gaps in Renode's stock `RenesasRA_GPT` so the HAL's GPT compare-match time driver ticks (CCMPA → ICU event `0x57`). Compiled at runtime via `include` (no Renode rebuild). |
| `Ra4m1Dac.cs` | DAC12 @ `0x4005_E000` (DADR0 read-back + DACR.DAOE0). |
| `Ra4m1Adc.cs` | ADC14 @ `0x4005_C000` (ADCSR.ADST self-clear, ADDR[0..14], S12ADI0 → ICU `0x29`). |
| `Ra4m1Spi.cs` | SPI0 full loopback (SPCR/SPSR/SPDR, SPTI/SPRI IRQs). |
| `Ra4m1FlashLp.cs` | FACI-LP flash P/E @ `0x407E_C000` (FENTRYR mode, FSTATR1.FRDY, FCR erase/program into backing code-flash). Models no P/E timing or fetch-stall hazard (bench-only). |
| `Ra4m1Can0.cs` | CAN0 @ `0x4005_0000` bridged to the stock `CANHub`. Frame-level, not bit-level. |
| `MockI2CSlave.cs` | Register-file I2C slave @ `0x50` for the RIIC scan/read-back cases. |
| `smoke` / `analog` / `serial` / `bootloader` `.robot` | The four pass/fail suites (run via `renode-test`) — 12 cases total, each mirroring a bench behaviour. |
| `smoke` / `analog` / `serial` / `bootloader` `.resc` | Generic loaders for manual/interactive runs. |

## Why the custom GPT model exists

Renode v1.16.1's stock `Timers.RenesasRA_GPT` cannot drive this firmware's time base. Three gaps
(verified against the stock model source and `crates/ra4m1-embassy/src/time_driver.rs`):

1. **`GTCR.CST` is inert** — the stock model only starts a channel on a `GTSTR` write; the HAL starts
   the counter with `GTCR.CST` (`time_driver.rs` `init`).
2. **`TPCS` is ignored** — cosmetic under virtual time.
3. **No `GTCCRA` compare-match** — the HAL alarm *is* the GTCCRA compare (`GTCCRA == GTCNT` →
   GPT0 CCMPA event `0x57` → ICU `IELSR0` → NVIC `IEL0`). The stock model never raises it, so the
   alarm never fires and the executor idles forever after the first `Timer`.

`Ra4m1Gpt320.cs` models GPT320 channel 0 as a **free-running 32-bit up-counter** (wraps
`0 → 0xFFFFFFFF → 0` without reset — the HAL's software 64-bit wrap-extension needs GTCNT to keep
running past every compare) plus a `GTCCRA` compare that **pulses** the CCMPA output (the ICU treats a
peripheral event as level-sensitive, so a rising edge latches/pends and the falling edge lets the
driver's `IELSR.IR` write-zero-to-clear stick). Built on Renode's `ComparingTimer`.

## Quick check — the smoke suite (automated pass/fail)

```sh
# From the repo root, in a checkout whose path has NO spaces or parentheses
# (Renode's monitor cannot tokenize those). CI checkout paths are clean.
cargo build --release -p ra4m1-hal --example blinky_gpio --example irq_smoke
<renode-install>/renode-test renode/smoke.robot
```

Expected:

```
+++++ Finished test 'smoke.Blinky Blinks The LED At 1 Hz'   ... status OK
+++++ Finished test 'smoke.Irq Smoke Heartbeat Advances'    ... status OK
Tests finished successfully :)
```

Both cases **fail** on the stock GPT model (negative control confirmed: LED stuck HIGH; RTT WrOff
frozen at `7 → 7`) and **pass** with `Ra4m1Gpt320`, so the assertions are meaningful.

## Running every suite locally (Windows, no CI)

The four suites — `smoke.robot`, `analog.robot`, `serial.robot`, `bootloader.robot` — run headless
against a pinned Renode. To reproduce a CI-equivalent run on a dev box:

1. **Renode 1.16.1 (pinned).** Download the official Windows portable build
   `renode-1.16.1.windows-portable-dotnet.zip` from the
   [v1.16.1 release](https://github.com/renode/renode/releases/tag/v1.16.1) and unzip it (no admin;
   the bundle ships its own .NET runtime). `renode-test` needs Python 3 with the test deps —
   `pip install robotframework==6.1 robotframework-retryfailed==0.2.0 psutil==5.9.3 pyyaml==6.0.* telnetlib3==2.0.*`
   into a Python the runner can see. Confirm `renode.exe --version` prints `Renode v1.16.1`.
2. **Build the ELFs the suites `LoadELF`** (from the repo root):
   ```sh
   cargo build --release --target thumbv7em-none-eabihf -p ra4m1-hal --examples
   cargo build --release --target thumbv7em-none-eabihf -p bootloader
   ```
3. **Run from a checkout path with NO spaces or parentheses.** Renode's monitor cannot tokenize
   `@paths` containing them, and the suites resolve their `.cs`/`.repl`/ELF assets through
   `${CURDIR}` (an absolute path). If your working copy lives under such a path (e.g.
   `…\Work ( Normal Corporation )\…`), run the suites from a clean-path **copy** of `renode/` plus
   the built `target/…` ELFs — a junction does **not** help because Robot resolves `${CURDIR}` back
   to the real path. Then:
   ```sh
   <renode-portable>/renode-test renode/smoke.robot renode/analog.robot renode/serial.robot renode/bootloader.robot
   ```
   All 12 cases pass; a wedged machine fails on the per-test timeout rather than hanging.

## Run it (manual / interactive)

```sh
cd <repo-root>
<renode-install>/renode --console --disable-xwt --plain \
  -e '$elf=@target/thumbv7em-none-eabihf/release/examples/blinky_gpio' \
  -e 'include @renode/smoke.resc' \
  -e 'start'
# blinky:   sysbus.port1.led_d13 State        (toggles ~1 Hz virtual)
# irq_smoke: sysbus ReadDoubleWord 0x2000002C (defmt-rtt WrOff, grows each heartbeat)
```

## What each test proves (and mirrors)

- **Blinky Blinks The LED At 1 Hz** — clocks → GPT time base → executor → task → GPIO. The D13/P111
  LED toggles at 1 Hz virtual (`Assert Led Is Blinking`). Mirrors bench **S2 "1 Hz blink"**.
- **Irq Smoke Heartbeat Advances** — a software-pended `IEL2` wakes a task, then the 1 s
  `embassy-time` heartbeat keeps logging: `defmt-rtt` WrOff strictly grows. Mirrors bench **S2
  "uptime heartbeat"** / spike **S-A** "time still ticking".

## Honesty boundaries — what this does NOT catch (stays the HIL bench's job)

- **Timing is virtual, not wall-clock.** The 3 MHz count frequency matches the HAL tick rate so blink
  is 1 Hz *in virtual seconds*, but Renode is functional, not cycle-accurate. No HOCO/clock tolerance,
  no `TPCS` divider, no timing margins.
- **No flash program/erase timing or the P/E fetch-stall hazard** (the reason `flash_lp.rs` is
  RAM-resident). A RAM-residency regression that would brick silicon passes silently in emulation.
- **No USB PHY/DFU, no CAN bit-timing, no real UART baud/framing, no analog accuracy.** The CAN0,
  SCI, ADC, and DAC models are **frame-/register-functional**, not electrical — the bench owns the
  physical layer.
- **Chip-fact BENCH-VERIFY items** are not exercised here, and a model coded from the same
  assumptions would pass circularly: the drivers' **ICU event numbers** and **SCI2 line settings**
  (plus option-bytes/OFS and oscillator behaviour) still need silicon confirmation.

A green run here complements — never replaces — the bench exit criteria.

## Remaining gaps (bench-owned)

Per-peripheral coverage is complete for the modelable subset. What stays off the emulator: the
end-to-end **`can_flash.py`** flash loop needs `CreateSocketCANBridge` (Linux-only) and is not wired
into CI — the `bootloader.robot` CAN case is frame-level (HELLO → reply) via the native `CANTester`.
**USB DFU** stays out of scope (Renode USB is export-only, no RA4M1 USBFS device model). See
[`TODO.md`](TODO.md) for the per-peripheral ledger.

## CI

All four suites run in CI as a **`renode` job** (`needs: firmware-build`) in the pinned
`antmicro/renode:1.16.1` container (`.github/workflows/ci.yml`), which rebuilds the `ra4m1-hal`
example + bootloader ELFs the suites `LoadELF`. The suites use `${CURDIR}`-relative absolute paths
(clean in a CI checkout), the C# models compile at runtime (no Renode rebuild), and each run is
headless, bounded (per-test timeout), and deterministic (virtual time). CI now runs only on `embassy-ra4m1-v*` tag
pushes and manual `workflow_dispatch`. Renode version pinned: **v1.16.1**.
