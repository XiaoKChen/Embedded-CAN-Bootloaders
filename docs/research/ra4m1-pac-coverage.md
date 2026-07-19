# RA4M1 PAC coverage audit (M3 spike S-B)

> Result of the M3.0 spike **S-B** (`docs/plan-m3-hal.md` §5). Determines, for
> every peripheral the from-scratch `crates/ra4m1-hal` needs, whether the
> `ra4m1` 0.2 PAC exposes a typed register block or whether a contained
> raw-MMIO module is required. Generated 2026-07-19 against `ra4m1` 0.2.0.

## Headline

**Every M3 peripheral is present and fully typed in the `ra4m1` 0.2 PAC — no
new raw-MMIO module (`pac_glue.rs`) is required for any of them.** The only
raw-MMIO seam in the HAL is pin-mux (PFS), which already exists in the base
crate (`crates/ra4m1-embassy/src/pfs.rs`) and is reused, not re-created.

## Coverage table

| Peripheral | PAC singleton(s) → block | Coverage | Driver approach |
|---|---|---|---|
| **GPIO PORT** | `PORT0..PORT9` → `port0` (`PODR/PDR/PIDR/PORR/POSR`) | Covered, typed | PAC typed regs at computed base `0x4004_0000 + 0x20·port` (contained, pfs.rs-style) |
| **PFS pin-mux** | `PFS` / `PMISC` (typed per-pin) | Narrow by design | Reuse base `ra4m1-embassy::pfs` raw-MMIO helper — no new seam |
| **GPT timers / PWM** | `GPT320/GPT321` (32-bit), `GPT162..GPT167` (16-bit) → `gpt320`/`gpt162` | Covered, typed | PAC driver |
| **SCI / UART** | `SCI0/SCI1/SCI2/SCI9` → `sci0`/`sci2` (incl. FIFO, `SEMR`/`MDDR`) | Covered, typed | PAC driver |
| **SPI** | `SPI0/SPI1` → `spi0`/`spi1` | Covered, typed | PAC driver |
| **RIIC / I2C** | `IIC0/IIC1` → `iic0`/`iic1` | Covered, typed | PAC driver (dedicated IIC — SCI-I2C fallback unnecessary) |
| **ADC14** | `ADC140` → `adc140` | Covered, typed | PAC driver |
| **DAC12** | `DAC12` → `dac12` | Covered, typed | PAC driver |

## Supporting facts resolved by the audit

- **GPT channel map** (plan open question Q7): **8 channels** — ch0 `GPT320`
  (32-bit, reserved for the `embassy-time` driver), ch1 `GPT321` (32-bit),
  ch2–ch7 `GPT162..GPT167` (16-bit).
- **ICU / NVIC**: `IELSR0..IELSR31` (32 NVIC lines). Slot 0 = time driver
  (priority `0xF0`, reserved); slot 1 = USBFS (reserved in the project table,
  not linked into HAL apps); **slots 2..=31 are claimable by HAL drivers**
  (default priority `0x80`).

## What this means for the peripheral phase

All M3 peripheral drivers are **PAC-based** (no raw-MMIO bring-up), so they are
mutually independent once the M3.0 scaffold exists and can be implemented in
parallel, each owning its own `crates/ra4m1-hal/src/<peripheral>.rs`:

- **M3.2 GPT / PWM** — independent; channel tokens ch1–ch7, `embedded-hal` `SetDutyCycle`.
- **M3.3 SCI / SPI / I2C** — gated on spike **S-C** (async serial: per-byte IRQ vs DTC), decided in `sci.rs` first; then `spi.rs` / `i2c.rs` parallelize. All use the S-A interrupt-binding macro (done in M3.0) for wakers.
- **M3.4 ADC / DAC** — `dac.rs` (blocking) is independent and trivial; `adc.rs` (async scan-complete) uses S-A once the async pattern settles.
