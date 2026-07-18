//! CAN update protocol session task: drives `boot_logic::protocol`'s effects
//! against the flash-LP and CAN0 drivers (U1.4, docs/plan.md §4/§6).
//!
//! Entry staging (order is load-bearing, SPEC §2.3):
//! 1. the transceiver rail (R33) and the per-device config (R34/R40) are
//!    brought up ONCE by `main::run_update_mode` — pre-executor, shared with
//!    `usb_dfu` (U2.3) — and handed in: this task gets a powered rail and a
//!    loaded [`DfuConfig`], and never re-reads the data-flash block;
//! 2. CAN0 init at 250 kbit/s, RX-filtered to the effective cmd ID. On
//!    failure: CAN-only mode resets back into the boot decision, parked
//!    mode degrades to (USB + LED)-only (R35 — no failure path dead-loops);
//! 3. serve forever. The bootloader is a **server**: it parks awaiting host
//!    commands by design — session awaits carry NO timeout; the host owns
//!    every retry/timeout budget (docs/plan.md §4, SPEC §4.8). The only
//!    exits are the GO reset (R13) and an external reset.
//!
//! Flash is the shared [`crate::FlashMutex`] (U2.3): every erase/program
//! acquires with `try_lock`, does one synchronous block op, and releases —
//! the guard is never held across an await, so `usb_dfu` and `led` stay live
//! between blocks (R39) and `try_lock` always succeeds on this one executor.
//!
//! Concurrency shape (docs/plan.md §4): RX is ticker-paced polling that
//! drains the whole 4-deep hardware FIFO each tick — loss-proof at
//! 250 kbit/s (see [`CAN_RX_POLL`]). Flash P/E and the bounded CAN TX poll
//! are the plan's two documented blocking exceptions. ERASE chunks one
//! block per task-loop iteration so the LED task stays live between blocks
//! (R21/R39); frames arriving mid-ERASE wait in the FIFO exactly as they
//! did in the C bootloader (the host stop-and-waits on the ERASE ACK).

use boot_logic::crc32::Crc32;
use boot_logic::dfu_config::{self, DfuConfig};
use boot_logic::image_meta::{ImageRead, MetadataRecord};
use boot_logic::memory_map::{APP_BASE, APP_BLOCK_COUNT, FLASH_BLOCK_SIZE, METADATA_ADDR};
use boot_logic::protocol::{build_info_reply, Effect, Engine, Event, Output};
use embassy_time::{Duration, Ticker, Timer};
use embedded_can::StandardId;

use crate::drivers::can0::{Bitrate, Can0, CanError, Frame};
use crate::flash_read::CodeFlashReader;
use crate::FlashMutex;

/// RX poll cadence. Sized for 250 kbit/s + the 4-deep hardware FIFO: a
/// DLC-8 frame occupies ≥ ~444 µs of wire time, so draining the whole FIFO
/// every 500 µs can never lose frames (docs/plan.md §4 — the named
/// deviation from interrupt-driven RX).
const CAN_RX_POLL: Duration = Duration::from_micros(500);

/// Blocks one ERASE covers: the metadata block FIRST (fail-closed ordering,
/// SPEC §4.5), then all 111 app-slot blocks (R21, plan §1.2 values).
const ERASE_TOTAL_BLOCKS: u32 = 1 + APP_BLOCK_COUNT;

/// Bytes hashed per VERIFY-CRC chunk between yields: ~0.3 ms of bitwise
/// CRC-32 at 48 MHz, so the LED fade never visibly stalls during VERIFY.
const CRC_CHUNK: usize = 256;

/// Awaited before terminal resets so the probe-rs host drains the RTT
/// buffer and the last log line survives.
const RTT_FLUSH_DELAY: Duration = Duration::from_millis(50);

/// How the bootloader entered update mode (SPEC §2.1) — decided by the boot
/// decision in `main.rs` and fixed for the life of the task.
#[derive(Debug, Clone, Copy, PartialEq, Eq, defmt::Format)]
pub enum EntryMode {
    /// `VBTBKR[4]` CAN magic: CAN is the only transport; a board that
    /// cannot bring CAN up is useless here → reset on init failure (R35).
    CanOnly,
    /// Double-tap magic or no valid app: recovery mode. CAN failure
    /// degrades to LED-only parking; never a dead board (R35).
    Parked,
}

/// Outcome of processing one engine [`Output`] chain.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum SessionExit {
    /// Keep serving.
    Continue,
    /// GO accepted: caller deinits CAN (R32) and resets (R13).
    Reset,
}

/// Session state the effect executor needs alongside the CAN driver. The
/// protocol window buffer is the [`Engine`]'s own 2 KB array; the engine
/// lives inside this task's future (a static task-pool slot), so the buffer
/// sits in SRAM — satisfying the flash driver's source-in-RAM requirement.
struct Session {
    engine: Engine,
    /// The shared flash driver (U2.3): locked per block op, never across an
    /// await (see the module docs / `crate::FlashMutex`).
    flash: &'static FlashMutex,
    /// Effective boot→host reply ID (replies + INFO frames), ≤ 0x7FF (R17).
    reply_id: StandardId,
    /// INFO product name (R25): non-empty only when the data-flash gate
    /// proved readability AND the config block parsed valid.
    info_name: [u8; dfu_config::NAME_LEN],
    info_name_len: u8,
    /// ERASE progress: `Some(n)` = next block to erase (0 = metadata block,
    /// 1..=111 = app block n−1); `None` = no erase in flight (R21 chunking).
    erase_next: Option<u32>,
}

#[embassy_executor::task]
pub async fn run(mode: EntryMode, flash: &'static FlashMutex, config: &'static DfuConfig) {
    defmt::info!("can_update: entry ({})", mode);

    // Rail (R33) and config (R34) were brought up by `main::run_update_mode`
    // and handed in (U2.3) — shared with `usb_dfu`, never re-read here.

    // CAN0 at the protocol bitrate, exact-match RX filter (R16/R29).
    // dfu_config guarantees IDs ≤ 0x7FF, so `new` cannot fail here.
    let cmd_id = StandardId::new(config.cmd_id).unwrap_or(StandardId::ZERO);
    let reply_id = StandardId::new(config.reply_id).unwrap_or(StandardId::ZERO);
    let mut can = match Can0::init(Bitrate::Kbit250, cmd_id) {
        Ok(can) => can,
        Err(err) => match mode {
            EntryMode::CanOnly => {
                // R35: reset back into the normal boot decision.
                defmt::error!(
                    "can_update: CAN init failed ({}) in CAN-only mode — resetting",
                    err
                );
                Timer::after(RTT_FLUSH_DELAY).await;
                cortex_m::peripheral::SCB::sys_reset();
            }
            EntryMode::Parked => {
                defmt::error!(
                    "can_update: CAN init failed ({}) — parked without CAN (R35)",
                    err
                );
                // R35: degrade — park this task forever (the LED task keeps
                // fading; recovery is an external reset). Typed as `Can0`
                // only to satisfy the match: the future never resolves.
                core::future::pending().await
            }
        },
    };

    let mut session = Session {
        engine: Engine::new(config.cmd_id),
        flash,
        reply_id,
        info_name: config.product,
        info_name_len: config.product_len,
        erase_next: None,
    };

    defmt::info!("can_update: serving at 250 kbit/s (RX poll 500 us)");

    // No timeout on any await below BY DESIGN: the device is the server and
    // parks forever awaiting commands; the host owns retries (plan §4).
    let mut ticker = Ticker::every(CAN_RX_POLL);
    'serve: loop {
        ticker.next().await;

        // One erase block per loop iteration (R21/R39): the ticker await
        // above is the yield that keeps the LED task live between blocks.
        // RX is deliberately NOT drained mid-ERASE (C parity): frames wait
        // in the 4-deep FIFO until the erase completes.
        if session.erase_next.is_some() {
            if let Some(out) = session.erase_step() {
                // Skip the ticker backlog the seconds-long erase built up.
                ticker.reset();
                if session.handle_output(&mut can, out).await == SessionExit::Reset {
                    break 'serve;
                }
            }
            continue 'serve;
        }

        // Drain the whole FIFO each tick (loss-proof: module docs).
        loop {
            match can.try_receive() {
                Ok(Some(frame)) => {
                    let out = session
                        .engine
                        .handle_frame(frame.id().as_raw(), frame.data());
                    if session.handle_output(&mut can, out).await == SessionExit::Reset {
                        break 'serve;
                    }
                }
                Ok(None) => break,
                Err(CanError::BusOff) => {
                    // R31: the driver already re-initialized itself; keep
                    // serving — never proceed with a half-programmed slot,
                    // never reset out of update mode on bus errors.
                    defmt::warn!("can_update: bus-off — re-initialized, still serving");
                    break;
                }
                Err(err) => {
                    defmt::warn!("can_update: rx error ({}) — still serving", err);
                    break;
                }
            }
        }
    }

    // GO accepted (SPEC §4.6): the ACK already went out in handle_output.
    // Deinit CAN (R32: module-stop + pins to reset state), then a clean
    // system reset re-enters the boot decision, which validates and boots
    // the new image (R13 — never a direct jump from update mode).
    defmt::info!("can_update: GO — deinit CAN and reset");
    can.deinit();
    Timer::after(RTT_FLUSH_DELAY).await;
    cortex_m::peripheral::SCB::sys_reset();
}

impl Session {
    /// Process one engine [`Output`]: transmit the reply (if any), perform
    /// the effect (if any), and feed completions back until the chain ends.
    /// Matches the engine's contract: effects complete before more frames
    /// are drained, so `ERR_STATE`-while-pending is unreachable in practice.
    async fn handle_output(&mut self, can: &mut Can0, mut out: Output) -> SessionExit {
        loop {
            if let Some(reply) = out.reply {
                self.send_reply(can, &reply);
            }
            let Some(effect) = out.effect else {
                return SessionExit::Continue;
            };
            out = match effect {
                Effect::EraseAll => {
                    // Deferred: erase_step() runs one block per task-loop
                    // iteration (R21) and feeds EraseDone when finished.
                    defmt::info!(
                        "can_update: ERASE start ({=u32} blocks, metadata first)",
                        ERASE_TOTAL_BLOCKS
                    );
                    self.erase_next = Some(0);
                    return SessionExit::Continue;
                }
                Effect::ProgramBlock { index } => {
                    let ok = self.program_window(index);
                    self.engine.handle(Event::ProgramDone(ok))
                }
                Effect::ComputeCrc { len } => {
                    let crc = compute_app_crc(len).await;
                    self.engine.handle(Event::CrcResult(crc))
                }
                Effect::WriteMetadata {
                    image_size,
                    image_crc,
                } => {
                    let ok = self.write_metadata(image_size, image_crc);
                    self.engine.handle(Event::MetadataDone(ok))
                }
                Effect::SendInfo => {
                    self.send_info(can);
                    return SessionExit::Continue;
                }
                Effect::Reset => return SessionExit::Reset,
            };
        }
    }

    /// One R21 erase chunk: erase the next block (metadata block first,
    /// SPEC §4.5), returning the engine's output when the sequence finishes
    /// or fails. `None` = more blocks remain (or nothing in flight).
    fn erase_step(&mut self) -> Option<Output> {
        let step = self.erase_next?;
        let addr = if step == 0 {
            METADATA_ADDR
        } else {
            APP_BASE + (step - 1) * FLASH_BLOCK_SIZE
        };
        // Acquire → one block erase → release. `try_lock` is total here:
        // guards are never held across an await, so no other task holds it.
        let result = {
            let Ok(mut flash) = self.flash.try_lock() else {
                self.erase_next = None;
                defmt::error!("can_update: flash busy during erase (unreachable on one executor)");
                return Some(self.engine.handle(Event::EraseDone(false)));
            };
            flash.erase_block(addr)
        };
        match result {
            Ok(()) => {
                let next = step + 1;
                if next == ERASE_TOTAL_BLOCKS {
                    self.erase_next = None;
                    defmt::info!("can_update: ERASE complete");
                    Some(self.engine.handle(Event::EraseDone(true)))
                } else {
                    self.erase_next = Some(next);
                    None
                }
            }
            Err(err) => {
                self.erase_next = None;
                defmt::error!("can_update: erase at {=u32:#x} failed ({})", addr, err);
                Some(self.engine.handle(Event::EraseDone(false)))
            }
        }
    }

    /// Program + read-back-verify the engine's tail-padded window at app
    /// block `index` (R22). One IRQ-masked RAM-resident critical section —
    /// safe because the windowed protocol guarantees no frame arrives
    /// mid-program (mid-window frames are unACKed; SPEC §9.1.4).
    fn program_window(&mut self, index: u32) -> bool {
        let addr = APP_BASE + index * FLASH_BLOCK_SIZE;
        let Ok(mut flash) = self.flash.try_lock() else {
            defmt::error!("can_update: flash busy during program (unreachable on one executor)");
            return false;
        };
        // engine.window() lives in this task's static future = SRAM, per
        // the driver's source-in-RAM requirement.
        match flash.program_verified(addr, self.engine.window()) {
            Ok(()) => true,
            Err(err) => {
                defmt::error!("can_update: program block {=u32} failed ({})", index, err);
                false
            }
        }
    }

    /// Commit point (SPEC §4.6, R15): build and program the 16-byte
    /// metadata record at `METADATA_ADDR` (erased earlier by EraseAll).
    fn write_metadata(&mut self, image_size: u32, image_crc: u32) -> bool {
        let record = MetadataRecord::for_image(image_size, image_crc).to_bytes();
        let Ok(mut flash) = self.flash.try_lock() else {
            defmt::error!("can_update: flash busy during metadata write (unreachable)");
            return false;
        };
        match flash.program_verified(METADATA_ADDR, &record) {
            Ok(()) => true,
            Err(err) => {
                defmt::error!("can_update: metadata write failed ({})", err);
                false
            }
        }
    }

    /// Transmit one reply frame on the effective reply ID. Loss is tolerated
    /// by design at every point: the host retries idempotent commands and
    /// HELLO restarts the session from any phase (SPEC §9.1.8).
    fn send_reply(&self, can: &mut Can0, payload: &[u8; 8]) {
        // Infallible: an 8-byte payload always fits a CAN 2.0 data frame.
        let Some(frame) = Frame::standard(self.reply_id, payload) else {
            return;
        };
        if let Err(err) = can.send(&frame) {
            defmt::warn!("can_update: reply tx failed ({}) — host retries", err);
        }
    }

    /// The 5-frame INFO reply (SPEC §4.7, R25): header + 32-byte
    /// 0x00-padded name, sent back-to-back (each TX bounded ≤ ~208 ms).
    fn send_info(&self, can: &mut Can0) {
        let len = usize::from(self.info_name_len).min(dfu_config::NAME_LEN);
        for payload in &build_info_reply(&self.info_name[..len]) {
            let Some(frame) = Frame::standard(self.reply_id, payload) else {
                return;
            };
            if let Err(err) = can.send(&frame) {
                defmt::warn!("can_update: INFO tx failed ({}) — host retries", err);
                return;
            }
        }
    }
}

/// Stream CRC-32 over code flash `[APP_BASE, APP_BASE + len)` (VERIFY,
/// SPEC §4.6), yielding between [`CRC_CHUNK`]-byte chunks so the LED fade
/// stays live through the ~0.3 s a full-slot hash takes.
async fn compute_app_crc(len: u32) -> u32 {
    let reader = CodeFlashReader;
    let mut crc = Crc32::new();
    let mut buf = [0u8; CRC_CHUNK];
    let mut offset = 0u32;
    while offset < len {
        // Lossless: take ≤ CRC_CHUNK (256).
        let take = (len - offset).min(CRC_CHUNK as u32);
        let chunk = &mut buf[..take as usize];
        if reader.read(APP_BASE + offset, chunk).is_err() {
            // Unreachable: the engine bounds len ≤ APP_SLOT_SIZE, keeping
            // the range inside code flash. Fail closed anyway: the partial
            // CRC cannot match, so VERIFY NAKs ERR_CRC (metadata untouched).
            defmt::error!("can_update: flash read failed during VERIFY CRC");
            break;
        }
        crc.update(chunk);
        offset += take;
        yield_now().await;
    }
    crc.finalize()
}

/// One-poll yield back to the executor (`embassy_futures::yield_now`'s
/// shape, hand-rolled because embassy-futures is not among this crate's
/// frozen dependencies): wakes itself immediately, so the task resumes on
/// the next executor pass after other ready tasks (the LED fade) have run.
fn yield_now() -> impl core::future::Future<Output = ()> {
    let mut yielded = false;
    core::future::poll_fn(move |cx| {
        if yielded {
            core::task::Poll::Ready(())
        } else {
            yielded = true;
            cx.waker().wake_by_ref();
            core::task::Poll::Pending
        }
    })
}
