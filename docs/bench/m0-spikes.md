# M0 bench checklist — spikes S0 / S2 / S5 on hardware

S1 (OFS emission) already passed host-side (byte-diff of `0x400..0x43F` against the
stock dump, enforced by linker ASSERTs). The remaining spike criteria need the board
(UNO R4 Minima) plus an SWD probe on the 10-pin connector.

One binary demonstrates everything: the current `bootloader` spike build.

## Setup

1. `cargo install probe-rs-tools`, wire the probe, confirm with `probe-rs list`
   (optional: `probe-rs info --chip R7FA4M1AB`).
2. **Recovery image first** — before the first flash, save the stock bootloader:

   ```sh
   git show main:docs/reference/dfu_minima.hex > dfu_minima.hex
   ```

   Restore command (if anything goes wrong):

   ```sh
   probe-rs download --chip R7FA4M1AB --binary-format hex dfu_minima.hex
   ```

## Run

3. `cargo run --release -p bootloader` (runner is preconfigured). Expected RTT:
   - `bootloader spike S0: HOCO 48 MHz up (ICLK 48 / PCLKA 48 / PCLKB 24 / FCLK 24)`
   - `SCKSCR=0x0 (expect 0x0 = HOCO), SCKDIVCR=0x10040100 (expect 0x10040100)`
   - `S5: VBTBKR[4..8]=0x0 — writing magic, then NVIC_SystemReset` (first run; the
     word may be non-zero garbage after a battery-domain POR)
   - After the automatic self-reset (~50 ms in): a second banner, then
     `S5: CAN-update magic survived the software reset` and
     `S5 PASS: magic seen exactly once, word reads 0 after clear`.
   - If probe-rs exits at the reset instead of riding it out:
     `probe-rs attach --chip R7FA4M1AB target/thumbv7em-none-eabihf/release/bootloader`
     — the post-reset logs are still in the RTT buffer.
   - Then S2 takes over: `S2: executor starting…`, the P111 LED toggling every
     500 ms (1 Hz blink), and an `S2 heartbeat: uptime N ms` line every 1 s.

## Pass criteria

- **S0:** both banner readbacks match; board still running after ≥ 30 s (that is
  also the on-hardware proof that OFS0 kept the IWDT off). Optional spot-check:
  `probe-rs read --chip R7FA4M1AB b32 0x400 4` →
  `FFFFFFFF FFFFCEDF 00FFFFFC 00FFFFFF`.
- **S5:** the PASS line appears exactly once (second boot on), word reads 0 after
  clear. Note: every fresh flash re-runs the write-magic → self-reset dance once.
- **S2:** LED blinks at a visibly regular 1 Hz (a steady LED means the executor or
  time driver is wedged). Over a timed ≥ 5 min window the printed uptime drifts
  from a wall clock by no more than ±1–2 % (HOCO tolerance). Optional wrap check:
  leave the board 25+ min and confirm the heartbeat ms value stays monotonic
  across the 23.9 min GPT320 counter wrap.
- **Stock-restore proof (S0 exit criterion):** run the restore command from step 2,
  power-cycle, double-tap reset → stock LED fade + DFU device enumerates. Re-flash
  the spike afterwards if continuing development.
