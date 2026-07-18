//! Bootloader entry point — the SPEC §2 boot decision, the `boot5` jump
//! sequence (R12), and per-mode task spawning (U1.3–U1.5).
//!
//! The boot decision runs as plain blocking code BEFORE the executor exists
//! (docs/plan.md §4): the app-boot path never pays executor startup and the
//! jump sequence never has to tear an executor down. The executor comes up
//! only for the two update modes, and once started it only ever dies by
//! reset — GO exits via `NVIC_SystemReset` (R13).
//!
//! Task spawn wiring (docs/plan.md §4):
//!
//! | Mode | Tasks |
//! |------|-------|
//! | CAN-only (`VBTBKR[4]` magic) | `can_update` |
//! | Parked (double-tap magic / no valid app) | `can_update` + `usb_dfu` + `led` fade |
//!
//! Parked mode (U2.3) runs USB DFU, the CAN engine, and the LED fade at once,
//! sharing one flash driver behind [`FlashMutex`]; CAN-only mode stays
//! CAN-only, exactly as M1 (SPEC §2.3 vs §2.5).

#![no_std]
#![no_main]

use defmt_rtt as _; // defmt transport over RTT
use panic_probe as _; // panic = defmt log, then reset

use boot_logic::dfu_config::{self, DfuConfig};
use boot_logic::image_meta;
use boot_logic::memory_map::APP_BASE;
use cortex_m_rt::entry;
use embassy_executor::Executor;
use embassy_sync::blocking_mutex::raw::NoopRawMutex;
use embassy_sync::mutex::Mutex;
use ra4m1_embassy::{clocks, time_driver, vbtbkr};
use static_cell::StaticCell;

use crate::drivers::flash_lp::FlashLp;
use crate::drivers::{usb_rail, usbfs};
use crate::flash_read::CodeFlashReader;
use crate::tasks::can_update::{self, EntryMode};
use crate::tasks::{led, usb_dfu};

mod board;
mod drivers;
mod flash_read;
mod ofs;
mod tasks;

/// Stock double-tap / DFU-detach handoff magic in `VBTBKR[0..4]`
/// (SPEC §1.4). Both magics live here rather than in `boot-logic` because
/// they parameterize only this boot decision — nothing pure consumes them.
const DOUBLE_TAP_MAGIC: u32 = 0x0773_8135;
/// App-requested CAN-update handoff magic in `VBTBKR[4..8]` (SPEC §1.4).
const CAN_UPDATE_MAGIC: u32 = 0xB007_CAFE;

/// SPEC §2 step 3 (R9): the double-tap arming window — 500 ms at ICLK
/// 48 MHz (`cortex_m::asm::delay` waits at least this many cycles).
const DOUBLE_TAP_WINDOW_CYCLES: u32 = clocks::ICLK_HZ / 2;

/// PRCR key + PRC1 (low-power/battery-backup unlock) and the bare re-lock
/// key, for the jump sequence's module-stop writes (SPEC §2.2, R8).
/// Duplicates `ra4m1_embassy::clocks`' crate-private constants — R01UM0007,
/// Register Write Protection chapter.
const PRCR_UNLOCK_LPM: u16 = 0xA502;
const PRCR_LOCK: u16 = 0xA500;

/// Thread-mode executor (the only one — nothing here has a hard latency
/// requirement, docs/plan.md §4). Task memory in embassy-executor 0.10 is a
/// compiler-sized static pool per `#[task]` fn — there is no `task-arena-size`
/// feature to tune (that was the pre-0.7 model the plan predated). Sizing is
/// therefore the per-task `pool_size`: `can_update`, `usb_dfu`, and `led` are
/// each single-instance, so all keep the default `pool_size = 1`. Three tasks
/// in parked mode, one in CAN-only.
static EXECUTOR: StaticCell<Executor> = StaticCell::new();

/// The shared flash driver type. `NoopRawMutex` is correct because the whole
/// bootloader runs on one thread-mode executor and **no ISR touches the flash
/// driver** — a `CriticalSectionRawMutex` would only add PRIMASK cost for no
/// safety (rules/embassy.md). Guards are never held across an await (flash
/// P/E is a synchronous critical section), so both `can_update` and `usb_dfu`
/// acquire with `try_lock`, which under a single cooperative executor always
/// succeeds: the only holder is a task between acquire and release, and the
/// executor never switches tasks there (no await in that span).
pub type FlashMutex = Mutex<NoopRawMutex, FlashLp>;

/// The one flash driver, shared by `can_update` and `usb_dfu` in parked mode
/// (docs/plan.md §4). Initialized once via `StaticCell` at update-mode entry.
static FLASH: StaticCell<FlashMutex> = StaticCell::new();

/// Per-device config (USB names + CAN IDs), loaded once behind the data-flash
/// gate (R34) and shared by both update tasks so the DFU config block at
/// `DFU_CONFIG_ADDR` is read **exactly once** (R37 — never twice).
static CONFIG: StaticCell<DfuConfig> = StaticCell::new();

#[entry]
fn main() -> ! {
    let board = board::init();
    defmt::info!("bootloader: HOCO 48 MHz up (ICLK 48 / PCLKA 48 / PCLKB 24 / FCLK 24)");

    // SPEC §2 step 1 (R6): double-tap / DFU magic → clear → parked mode.
    if vbtbkr::read_word(&board.system, vbtbkr::DOUBLE_TAP_WORD_OFFSET) == DOUBLE_TAP_MAGIC {
        vbtbkr::clear_word(&board.system, vbtbkr::DOUBLE_TAP_WORD_OFFSET);
        defmt::info!("boot: double-tap magic — entering parked mode");
        run_update_mode(board, EntryMode::Parked);
    }

    // SPEC §2 step 2 (R7): app-requested CAN magic → clear → CAN-only mode.
    if vbtbkr::read_word(&board.system, vbtbkr::CAN_UPDATE_WORD_OFFSET) == CAN_UPDATE_MAGIC {
        vbtbkr::clear_word(&board.system, vbtbkr::CAN_UPDATE_WORD_OFFSET);
        defmt::info!("boot: CAN-update magic — entering CAN-only update mode");
        run_update_mode(board, EntryMode::CanOnly);
    }

    // SPEC §2 step 3 (R9): a non-power-on reset arms the double-tap window —
    // magic in, 500 ms wait, magic out. A second reset tap inside the window
    // leaves the magic set and re-enters step 1 on the next boot. The
    // busy-wait is the documented pre-executor exception (docs/plan.md §4):
    // no executor exists yet, so there is nothing to starve.
    if board.system.rstsr0.read().porf().bit_is_clear() {
        vbtbkr::write_word(
            &board.system,
            vbtbkr::DOUBLE_TAP_WORD_OFFSET,
            DOUBLE_TAP_MAGIC,
        );
        cortex_m::asm::delay(DOUBLE_TAP_WINDOW_CYCLES);
        vbtbkr::clear_word(&board.system, vbtbkr::DOUBLE_TAP_WORD_OFFSET);
    }

    // SPEC §2 step 4 (R10/R11): full metadata validation with the stock
    // SP-word fallback for absent/corrupt metadata only. Read failures and
    // every other validation failure fail CLOSED into the parked bootloader
    // (LED fading, CAN listening) — never a dead board.
    match image_meta::app_bootable(&CodeFlashReader) {
        Ok(true) => jump_to_app(&board.system), // SPEC §2 step 5: valid → jump
        Ok(false) => defmt::warn!("boot: no valid app — parking (fail closed)"),
        Err(_) => defmt::warn!("boot: flash read failed during validation — parking (fail closed)"),
    }
    run_update_mode(board, EntryMode::Parked)
}

/// Bring the shared resources up, then hand control to the executor forever
/// (it WFE-sleeps between wakes; once started it only ever dies by reset,
/// docs/plan.md §4). Spawns per the mode table in the module docs.
///
/// The rail power-up, flash-driver take, and config load all happen here —
/// once, pre-executor — so both update tasks share them deterministically:
///
/// - **Rail first (SPEC §5.3, R33):** on custom boards the CAN transceiver
///   *and* the USBFS device both hang off VCC_USB. Powering it here (not
///   inside a task) guarantees a live rail before either `can_update`'s CAN
///   init or `usb_dfu`'s `Bus::enable` runs — no fragile spawn-order
///   dependency. `usb_rail` uses cycle-counted delays (no `embassy-time`), so
///   it is a legal pre-executor blocking one-shot (docs/plan.md §4).
/// - **Config once (R34/R37):** the data-flash gate + config-block read run
///   once; `can_update` takes its CAN IDs and `usb_dfu` its USB name strings
///   from the same `DfuConfig`, so the block is never read twice.
fn run_update_mode(board: board::Board, mode: EntryMode) -> ! {
    // Stage 1 (R33): power the transceiver/USB rail BEFORE any CAN or USB
    // register access — an unpowered rail makes the CAN controller deaf/mute
    // (SPEC §9.1.1). One-shot ~5 ms blocking bring-up (documented exception).
    usb_rail::power_up_transceiver_rail();
    defmt::info!("boot: transceiver/USB rail powered");

    // Stage 2 (R34): the one flash driver, and the per-device config loaded
    // once behind the data-flash gate (shared by both tasks — never re-read).
    let mut flash =
        FlashLp::take().expect("FlashLp::take: driver already taken — run_update_mode re-entered");
    let config: &'static DfuConfig = CONFIG.init(load_config(&mut flash));
    defmt::info!(
        "boot: config cmd_id={=u16:#x} reply_id={=u16:#x} name_len={=u8}",
        config.cmd_id,
        config.reply_id,
        config.product_len
    );
    let flash: &'static FlashMutex = FLASH.init(Mutex::new(flash));

    time_driver::init();
    let executor = EXECUTOR.init(Executor::new());
    executor.run(move |spawner| {
        // Spawn failures are init errors — fail loudly (rules/embassy.md).
        // In embassy-executor 0.10 the task-pool claim is the fallible step.
        spawner.spawn(
            can_update::run(mode, flash, config).expect("can_update pool empty: spawned twice"),
        );
        if mode == EntryMode::Parked {
            // USB DFU + LED fade are parked-mode only (SPEC §2.3 vs §2.5): the
            // C bootloader's CAN-only path ran neither. Both share the flash
            // mutex; `usb_dfu` also shares the config strings.
            let driver = usbfs::Driver::new(board.usbfs);
            spawner.spawn(
                usb_dfu::run(driver, flash, config).expect("usb_dfu pool empty: spawned twice"),
            );
            spawner.spawn(
                led::fade(board.gpt163, board.pfs, board.pmisc)
                    .expect("led pool empty: spawned twice"),
            );
        }
    })
}

/// Data-flash gate + config-block load (R34/R40), run once at update-mode
/// entry so `can_update` and `usb_dfu` share one [`DfuConfig`]. The ONLY
/// proof of data-flash readability is the gate; unproven/unreadable/invalid
/// all collapse to the compiled defaults (IDs `0x79E`/`0x79F`, empty names)
/// and the board keeps serving — never a locked core (SPEC §2.4, §9.1.2).
fn load_config(flash: &mut FlashLp) -> DfuConfig {
    if !flash.data_flash_enable() {
        defmt::warn!("boot: data flash unproven (R34) — compiled config defaults");
        return DfuConfig::default();
    }
    defmt::info!("boot: data-flash gate passed");
    let mut block = [0u8; dfu_config::V2_BLOCK_LEN];
    match flash.read_data_flash(dfu_config::DFU_CONFIG_ADDR, &mut block) {
        Ok(()) => dfu_config::effective_config(Some(&block)),
        Err(err) => {
            defmt::warn!("boot: config read failed ({}) — compiled defaults", err);
            DfuConfig::default()
        }
    }
}

/// SPEC §2.2 app entry (`boot5`, R12), ported for `APP_BASE = 0x8000`
/// (plan §1.2 supersedes R5/R12's `0x4000`).
///
/// Steps the C original performs that need **no code** on this path:
///
/// - **CAN deinit:** CAN only ever comes up inside the update modes, which
///   exit exclusively via `NVIC_SystemReset` after `Can0::deinit`
///   (R13/R32) — this function is unreachable with CAN initialized.
/// - **Time-driver stop (the plan §4 R12 extension):** `time_driver::init`
///   is called only by `run_update_mode`, never on the jump path, so GPT320
///   is still module-stopped and IEL0 masked here — the "stop the
///   time-driver counter" requirement holds by construction (see
///   `time_driver.rs`'s lifecycle contract). No stop API was needed.
fn jump_to_app(system: &ra4m1::SYSTEM) -> ! {
    defmt::info!("boot: valid app — jumping to {=u32:#x}", APP_BASE);

    // SAFETY: transient PAC/core singleton re-acquisition on the one-way
    // path out of the bootloader; nothing else runs (pre-executor).
    let p = unsafe { ra4m1::Peripherals::steal() };
    let cp = unsafe { cortex_m::Peripherals::steal() };

    // Module-stop USBFS under the PRC1 unlock, C parity (SPEC §2.2). The C
    // also stopped USBHS, which does not exist on the RA4M1 — there is no
    // register to write for that half. On this path the rail power-up never
    // ran, so this usually re-asserts the reset state.
    // SAFETY: documented PRCR key writes (R01UM0007, Register Write
    // Protection); MSTPCRB RMW cannot race pre-executor.
    system.prcr.write(|w| unsafe { w.bits(PRCR_UNLOCK_LPM) });
    p.MSTP.mstpcrb.modify(|_, w| w.mstpb11()._1());
    let _ = p.MSTP.mstpcrb.read(); // completion read-back
                                   // SAFETY: PRCR re-lock (key only).
    system.prcr.write(|w| unsafe { w.bits(PRCR_LOCK) });

    // Disable MSP monitoring (stock behavior kept: `R_MPU_SPMON->SP[0].CTL
    // = 0`, SPEC §2.2).
    // SAFETY: whole-register 0 = monitor disabled (R01UM0007, Stack Pointer
    // Monitor); this project never enables it.
    p.SPMON.mspmpuctl.write(|w| unsafe { w.bits(0) });

    // IRQs off + barriers while the interrupt fabric is torn down and
    // retargeted (SPEC §2.2 order).
    cortex_m::interrupt::disable();
    cortex_m::asm::dsb();
    cortex_m::asm::isb();

    // SysTick off (stock behavior kept — this bootloader never starts it;
    // its time base is GPT320, which never ran on this path).
    // SAFETY: SYST.CSR = 0 disables the counter and its interrupt.
    unsafe { (*cortex_m::peripheral::SYST::PTR).csr.write(0) };

    // NVIC teardown: disable + clear-pending every implemented line (the
    // RA4M1's ICU exposes 32, all in word 0) before handing the vector
    // table to the app. Nothing is enabled on this path — the executor
    // never started — so this re-asserts reset state, cheaply.
    // SAFETY: whole-register writes; W1C semantics for both registers.
    unsafe {
        cp.NVIC.icer[0].write(u32::MAX);
        cp.NVIC.icpr[0].write(u32::MAX);
    }

    // VTOR → the app's vector table: the BOOTLOADER sets it, the app need
    // not (R5 policy, value per plan §1.2).
    // SAFETY: APP_BASE is 128-byte aligned (32 KB) as VTOR requires; the
    // barriers order the retarget before the jump.
    unsafe { (*cortex_m::peripheral::SCB::PTR).vtor.write(APP_BASE) };
    cortex_m::asm::dsb();
    cortex_m::asm::isb();

    // DEVIATION from SPEC §2.2 ("IRQs left disabled for the app"): PRIMASK
    // is cleared before the jump. FSP-based apps re-enable interrupts in
    // SystemInit, but cortex-m-rt/Embassy apps (demo-app) do not — jumping
    // masked would leave the app's time-driver ISR dead and its executor
    // spinning on WFE. Safe here: every NVIC line is disabled and
    // pending-cleared and SysTick is off, so nothing can preempt the app's
    // startup before it configures its own sources.
    // SAFETY: no enabled interrupt sources exist (torn down above).
    unsafe { cortex_m::interrupt::enable() };

    // MSP ← [APP_BASE], PC ← [APP_BASE + 4] (Thumb), never returns.
    // SAFETY: validation proved the SP word points into SRAM and the reset
    // vector is a Thumb address inside the app slot (SPEC §3.3 steps 4–5;
    // the stock fallback checks the SP word's SRAM high byte).
    unsafe { cortex_m::asm::bootload(APP_BASE as *const u32) }
}
