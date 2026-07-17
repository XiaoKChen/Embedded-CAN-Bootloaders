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
//! | Parked (double-tap magic / no valid app) | `can_update` + `led` fade |

#![no_std]
#![no_main]

use defmt_rtt as _; // defmt transport over RTT
use panic_probe as _; // panic = defmt log, then reset

use boot_logic::image_meta;
use boot_logic::memory_map::APP_BASE;
use cortex_m_rt::entry;
use embassy_executor::Executor;
use ra4m1_embassy::{clocks, time_driver, vbtbkr};
use static_cell::StaticCell;

use crate::flash_read::CodeFlashReader;
use crate::tasks::can_update::{self, EntryMode};
use crate::tasks::led;

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
/// compiler-sized static pool per `#[task]` fn (pool_size 1 each), so the
/// RAM budget is exact per task with no arena to tune.
static EXECUTOR: StaticCell<Executor> = StaticCell::new();

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

/// Bring the time base up and hand control to the executor forever (it
/// WFE-sleeps between wakes; once started it only ever dies by reset,
/// docs/plan.md §4). Spawns per the mode table in the module docs.
fn run_update_mode(board: board::Board, mode: EntryMode) -> ! {
    time_driver::init();
    let executor = EXECUTOR.init(Executor::new());
    executor.run(move |spawner| {
        // Spawn failures are init errors — fail loudly (rules/embassy.md).
        // In embassy-executor 0.10 the task-pool claim is the fallible step.
        spawner.spawn(can_update::run(mode).expect("can_update pool empty: spawned twice"));
        if mode == EntryMode::Parked {
            // LED fade is parked-mode only (SPEC §2.3 vs §2.5): the C
            // bootloader's CAN-only path never ran the LED task.
            spawner.spawn(
                led::fade(board.gpt163, board.pfs, board.pmisc)
                    .expect("led pool empty: spawned twice"),
            );
        }
    })
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
