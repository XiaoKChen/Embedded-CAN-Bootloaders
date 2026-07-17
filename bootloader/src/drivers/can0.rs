//! Register-level CAN0 driver for the RA4M1 — FIFO-mailbox mode, polling
//! only (SPEC §5.2, R27–R32).
//!
//! Faithful port of the bench-proven C driver (`main:src/drivers/
//! can_polled.c`); every register step is commented against SPEC §5.2's
//! recipe so the port stays auditable line-by-line. Register/bit facts:
//! R01UM0007 (RA4M1 HW manual), CAN chapter (§30).
//!
//! Concurrency contract (docs/plan.md §4): RX is non-blocking
//! ([`Can0::try_receive`], one frame per call) and ticker-paced by the
//! caller, which drains the 4-deep hardware FIFO each tick. TX
//! ([`Can0::send`]) is a bounded synchronous poll (≤ ~200 ms worst case) —
//! one of the plan's two documented blocking exceptions. No `embassy-time`
//! in this driver: every bound is a cycle-counted loop, so spike S4 can
//! call it pre-executor.
use embedded_can::{Id, StandardId};
use ra4m1_embassy::{clocks, pfs};

// -----------------------------------------------------------------------------
// Constants (SPEC §5.2 configuration table)
// -----------------------------------------------------------------------------

/// PFS PSEL code routing a pin to CAN0: 0x10 into PSEL\[28:24\] (SPEC §5.2).
const PSEL_CAN: u8 = 0x10;
/// D4 = P103 = CTX0 (TX) — ArduinoCore-renesas MINIMA pinmux (SPEC §5.2).
const CAN_TX_PORT: u8 = 1;
const CAN_TX_PIN: u8 = 3;
/// D5 = P102 = CRX0 (RX).
const CAN_RX_PORT: u8 = 1;
const CAN_RX_PIN: u8 = 2;

/// STR mode-status bits RSTST/HLTST/SLPST (bits 8–10): the requested mode
/// is reached when `STR & 0x0700 == CANM << 8` (SPEC §5.2 mode sequencing).
const STR_MODE_MASK: u16 = 0x0700;

/// 16 tq/bit: SS = 1 + TSEG1 = 11 + TSEG2 = 4 (SJW = 1) → sample point
/// (1 + 11) / 16 = 75 % (SPEC §5.2 bit timing; chosen for HOCO ±~1 %
/// tolerance margin, SPEC §9.1.5).
const TQ_PER_BIT: u32 = 16;
/// BCR.BRP prescaler *value* for 250 kbit/s (register field holds value−1):
/// 24 MHz PCLKB / 6 = 4 MHz tq clock; 4 MHz / 16 tq = 250 kbit/s.
const BRP_250K: u16 = 6;
/// BCR.BRP prescaler value for 500 kbit/s:
/// 24 MHz PCLKB / 3 = 8 MHz tq clock; 8 MHz / 16 tq = 500 kbit/s.
const BRP_500K: u16 = 3;
// Compile-time proof the prescalers produce the advertised bitrates from
// the project clock tree (PCLKB 24 MHz, BCR.CCLKS = 0 — clocks.rs).
const _: () = assert!(clocks::PCLKB_HZ / (BRP_250K as u32) / TQ_PER_BIT == 250_000);
const _: () = assert!(clocks::PCLKB_HZ / (BRP_500K as u32) / TQ_PER_BIT == 500_000);

/// TX uses normal mailbox 0 (SPEC §5.2 mailbox scheme).
const TX_MAILBOX: usize = 0;
/// Mailboxes 0–23 are the normal mailboxes in FIFO mode; 28–31 are the
/// 4-deep RX FIFO, read through the MB28 access window (SPEC §5.2).
const NORMAL_MAILBOXES: usize = 24;
/// MKIVLR bits 24–31 must stay 0 in FIFO mailbox mode (SPEC §5.2); bits
/// 0–23 = 1 make the (disabled) normal mailboxes ignore the masks.
const MKIVLR_FIFO_MASK: u32 = 0x00FF_FFFF;
/// Exact-match acceptance: MKR\[6\]/MKR\[7\] compare every SID bit.
const SID_MASK: u16 = 0x7FF;
/// RFCR value enabling the RX FIFO with RFMLF clear (`RFE` = bit 0).
const RFCR_RFE: u8 = 0x01;
/// Writing 0xFF to RFPCR advances the CPU-side FIFO pointer (SPEC §5.2).
const RFPCR_ADVANCE: u8 = 0xFF;
/// CAN 2.0 payload ceiling; DLC > 8 on the wire is clamped to this.
const FRAME_DATA_MAX: u8 = 8;

/// Mode-switch poll bound — C parity (`CAN_MODE_TIMEOUT_LOOPS`): a mode
/// change takes a few CAN-clock cycles, so 100k polls at ICLK 48 MHz is
/// orders of magnitude of margin.
const MODE_TIMEOUT_POLLS: u32 = 100_000;
/// SENTDATA poll bound — C parity (`CAN_TX_TIMEOUT_LOOPS`): each poll
/// iteration costs ≥ ~5 CPU cycles at `clocks::ICLK_HZ` (48 MHz), so 2M
/// polls bound the wait to roughly ≥ 208 ms — enough for arbitration loss
/// plus error retries at 250 kbit/s (SPEC §5.2 "TX bounded poll").
const TX_TIMEOUT_POLLS: u32 = 2_000_000;

// -----------------------------------------------------------------------------
// Public types
// -----------------------------------------------------------------------------

/// Accepted bus bitrates (SPEC §5.2: only these two; the C driver rejected
/// anything else at run time — the enum makes it unrepresentable).
#[derive(Debug, Clone, Copy, PartialEq, Eq, defmt::Format)]
pub enum Bitrate {
    /// 250 kbit/s — Protocol v1 default (R25).
    Kbit250,
    /// 500 kbit/s — supported but not the default (SPEC §9.1.5).
    #[allow(dead_code)]
    // Protocol v1 always runs 250 k; kept per SPEC §5.2's two-rate contract
    Kbit500,
}

impl Bitrate {
    /// BCR.BRP prescaler value (not the value−1 register encoding).
    const fn prescaler(self) -> u16 {
        match self {
            Bitrate::Kbit250 => BRP_250K,
            Bitrate::Kbit500 => BRP_500K,
        }
    }
}

/// Driver failure modes. Mirrors the C `can_polled_status_t` error family:
/// "no data" is `Ok(None)` from [`Can0::try_receive`], and the C driver's
/// bad-argument errors are unrepresentable here ([`Bitrate`]/[`StandardId`]/
/// [`Frame`] enforce them by type).
#[derive(Debug, Clone, Copy, PartialEq, Eq, defmt::Format)]
pub enum CanError {
    /// STR never mirrored the requested CTLR.CANM mode within the poll
    /// bound (init/reinit failure).
    ModeSwitch,
    /// SENTDATA never set within the ~200 ms bound; the transmit request
    /// was aborted (frame not sent — host retries cover this, SPEC §9.1.8).
    TxTimeout,
    /// Bus-off was detected; the driver has already re-initialized itself
    /// (R31). Keep waiting / retrying — never proceed with a
    /// half-programmed slot (SPEC §9.1.10).
    BusOff,
}

/// One CAN 2.0A data frame: 11-bit standard ID, 0–8 data bytes — the only
/// shape Protocol v1 uses. Extended IDs and remote frames are rejected at
/// construction, exactly like the C driver.
///
/// Invariant: `dlc <= 8` and `data[dlc..]` is always zero (canonical form,
/// so `PartialEq` is meaningful).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Frame {
    id: StandardId,
    dlc: u8,
    data: [u8; 8],
}

impl Frame {
    /// Build a data frame. Returns `None` if `data.len() > 8`.
    pub fn standard(id: StandardId, data: &[u8]) -> Option<Self> {
        if data.len() > usize::from(FRAME_DATA_MAX) {
            return None;
        }
        let dlc = u8::try_from(data.len()).ok()?; // infallible after the guard
        let mut bytes = [0u8; 8];
        bytes[..data.len()].copy_from_slice(data);
        Some(Self {
            id,
            dlc,
            data: bytes,
        })
    }

    /// 11-bit standard identifier.
    pub fn id(&self) -> StandardId {
        self.id
    }

    /// Payload bytes (`dlc` long, 0..=8).
    pub fn data(&self) -> &[u8] {
        &self.data[..usize::from(self.dlc)]
    }
}

impl embedded_can::Frame for Frame {
    fn new(id: impl Into<Id>, data: &[u8]) -> Option<Self> {
        match id.into() {
            Id::Standard(sid) => Self::standard(sid, data),
            // 11-bit-only driver (SPEC §5.2) — extended IDs unsupported.
            Id::Extended(_) => None,
        }
    }

    fn new_remote(_id: impl Into<Id>, _dlc: usize) -> Option<Self> {
        // Protocol v1 never uses remote frames; the C driver always sent
        // RTR = 0 and could not express one either.
        None
    }

    fn is_extended(&self) -> bool {
        false
    }

    fn is_remote_frame(&self) -> bool {
        false
    }

    fn id(&self) -> Id {
        Id::Standard(self.id)
    }

    fn dlc(&self) -> usize {
        usize::from(self.dlc)
    }

    fn data(&self) -> &[u8] {
        Frame::data(self)
    }
}

impl defmt::Format for Frame {
    fn format(&self, f: defmt::Formatter) {
        defmt::write!(
            f,
            "Frame(id={=u16:#x}, data={=[u8]:x})",
            self.id.as_raw(),
            self.data()
        );
    }
}

// -----------------------------------------------------------------------------
// Driver
// -----------------------------------------------------------------------------

/// CTLR.CANM encodings (SPEC §5.2: 00 = operate, 01 = reset, 10 = halt).
#[derive(Clone, Copy, PartialEq, Eq)]
enum CanMode {
    Operate,
    Reset,
    Halt,
}

impl CanMode {
    /// Expected `STR & STR_MODE_MASK` once this mode is reached (CANM << 8).
    fn str_bits(self) -> u16 {
        match self {
            CanMode::Operate => 0x0000,
            CanMode::Reset => 0x0100,
            CanMode::Halt => 0x0200,
        }
    }
}

/// CAN0 in FIFO-mailbox mode with an exact single-ID acceptance filter.
/// Owns the CAN0 register block from [`Can0::init`] until [`Can0::deinit`]
/// consumes it.
pub struct Can0 {
    can: ra4m1::CAN0,
    bitrate: Bitrate,
    rx_id: StandardId,
}

impl Can0 {
    /// Bring CAN0 up at `bitrate`, receiving ONLY frames whose 11-bit ID
    /// equals `rx_id` (4-deep hardware RX FIFO). Requires the clock tree
    /// from `clocks::init_hoco_48mhz` (PCLKB 24 MHz, PCLKA:PCLKB = 2:1 —
    /// the CAN constraint on this part) and, on boards that feed the
    /// transceiver from VCC_USB, `usb_rail::power_up_transceiver_rail()`
    /// BEFORE this call (SPEC §5.3).
    ///
    /// Fails with [`CanError::ModeSwitch`] if the controller never reaches
    /// a requested mode. Keep at most one live driver: the returned value
    /// owns CAN0 until `deinit` consumes it.
    pub fn init(bitrate: Bitrate, rx_id: StandardId) -> Result<Self, CanError> {
        // SAFETY: acquisition of the CAN0 singleton (the checked `take()`
        // is not compiled in — see board.rs). This module is the only CAN0
        // user in the crate, and the one-live-driver contract documented
        // above prevents aliased handles.
        let can = unsafe { ra4m1::Peripherals::steal() }.CAN0;
        let driver = Can0 {
            can,
            bitrate,
            rx_id,
        };
        driver.init_hw()?;
        Ok(driver)
    }

    /// The full SPEC §5.2 bring-up recipe. Also the R31 bus-off recovery
    /// path: entering reset mode clears the latched bus-off condition and
    /// the error counters.
    fn init_hw(&self) -> Result<(), CanError> {
        // SAFETY: transient re-acquisition of the PAC singletons for MSTP
        // and the P102/P103 pin mux. MSTPCRB's read-modify-write cannot
        // race: every MSTPCRB user (this driver, drivers::usb_rail) runs
        // sequentially in the same boot/task context and no ISR touches
        // it. PFS/PMISC writes go through pfs.rs' PWPR protocol and touch
        // only P102/P103, which no other module claims; the thread-mode
        // executor cannot preempt a task mid-sequence (no awaits here).
        let p = unsafe { ra4m1::Peripherals::steal() };

        // SPEC §5.2 "module start": release CAN0 module-stop (MSTPCRB
        // bit 2); the dummy read-back completes the write before the first
        // CAN register access.
        p.MSTP.mstpcrb.modify(|_, w| w.mstpb2()._0());
        let _ = p.MSTP.mstpcrb.read();

        // SPEC §5.2 "pin routing": PSEL = 0x10 then PMR, PSEL-before-PMR
        // ordering handled by pfs.rs (built for this in U0.2).
        pfs::set_peripheral_function(&p.PFS, &p.PMISC, CAN_TX_PORT, CAN_TX_PIN, PSEL_CAN);
        pfs::set_peripheral_function(&p.PFS, &p.PMISC, CAN_RX_PORT, CAN_RX_PIN, PSEL_CAN);

        // SPEC §5.2 "reset mode": MBM/IDFM/MLM and BCR are writable only
        // here; entering reset also clears latched bus-off + error
        // counters (which is why reinit() lands back in this function).
        self.set_mode(CanMode::Reset)?;

        // SPEC §5.2 "MBM FIFO mode": FIFO mailbox mode (MBM = 1), standard
        // ID (IDFM = 00), overwrite on FIFO full (MLM = 0), stay in reset
        // while writing. Full-register write — C parity with
        // `CTLR = MBM | (RESET << CANM_Pos)` = 0x0101; the PAC's `write`
        // bases unset fields on CTLR's 0x0500 reset value, so SLPM is
        // cleared explicitly.
        self.can.ctlr.write(|w| {
            w.mbm()
                ._1()
                .idfm()
                ._00()
                .mlm()
                ._0()
                .canm()
                ._01()
                .slpm()
                ._0()
        });

        // SPEC §5.2 "bit timing" (fields hold value−1): CCLKS = 0 selects
        // PCLKB, BRP per bitrate, TSEG1 = 11 tq, TSEG2 = 4 tq, SJW = 1 tq.
        let brp_field = self.bitrate.prescaler() - 1;
        self.can.bcr.write(|w| {
            // SAFETY: BRP is a 10-bit field; prescaler − 1 ∈ {2, 5}.
            unsafe { w.brp().bits(brp_field) }
                .tseg1()
                ._1010() // 11 Tq
                .tseg2()
                ._011() // 4 Tq
                .sjw()
                ._00() // 1 Tq
                .cclks()
                ._0() // PCLKB
        });

        // SPEC §5.2 "halt-mode mailbox config": mailbox controls, masks
        // and the FIFO ID-compare registers are configured in halt mode.
        self.set_mode(CanMode::Halt)?;

        // Disable the 24 normal mailboxes and clear stale flags (C parity:
        // `MCTL_RX[i] = 0`).
        for mctl in &self.can.mctl_rx()[..NORMAL_MAILBOXES] {
            // SAFETY: whole-register 0 = mailbox disabled, all flags clear.
            mctl.write(|w| unsafe { w.bits(0) });
        }

        // SPEC §5.2 "FIDCR/MKR exact-ID filter": both RX-FIFO ID-compare
        // registers hold the same standard ID (IDE = 0, RTR = 0) and both
        // FIFO masks (MKR[6]/MKR[7]) compare every SID bit — the FIFO
        // accepts ONLY `rx_id`.
        let sid = self.rx_id.as_raw();
        for fidcr in &self.can.fidcr {
            // SAFETY: SID is an 11-bit field; StandardId ≤ 0x7FF by type.
            fidcr.write(|w| unsafe { w.sid().bits(sid) }.ide()._0().rtr()._0());
        }
        for mkr in &self.can.mkr[6..8] {
            // SAFETY: SID is an 11-bit field; SID_MASK = 0x7FF.
            mkr.write(|w| unsafe { w.sid().bits(SID_MASK) });
        }
        // SAFETY: documented whole-register value — bits 24–31 must stay 0
        // in FIFO mailbox mode (SPEC §5.2).
        self.can
            .mkivlr
            .write(|w| unsafe { w.bits(MKIVLR_FIFO_MASK) });

        // Poll-only driver: no mailbox or error interrupts (SPEC §5.2).
        // SAFETY: whole-register 0 = every interrupt source disabled.
        self.can.mier().write(|w| unsafe { w.bits(0) });
        self.can.eier.write(|w| unsafe { w.bits(0) });

        // SPEC §5.2 "operate mode".
        self.set_mode(CanMode::Operate)?;

        // SPEC §5.2 "RFE enable": the RX FIFO may only be enabled after
        // entering operate mode.
        // SAFETY: 0x01 = RFE set, RFMLF clear — C parity (`RFCR = RFE`).
        self.can.rfcr.write(|w| unsafe { w.bits(RFCR_RFE) });

        Ok(())
    }

    /// SPEC §5.2 mode sequencing: clear SLPM, request CTLR.CANM, poll STR
    /// until RSTST/HLTST/SLPST mirror the request (bounded).
    fn set_mode(&self, mode: CanMode) -> Result<(), CanError> {
        self.can.ctlr.modify(|_, w| {
            let w = w.slpm()._0();
            match mode {
                CanMode::Operate => w.canm()._00(),
                CanMode::Reset => w.canm()._01(),
                CanMode::Halt => w.canm()._10(),
            }
        });
        for _ in 0..MODE_TIMEOUT_POLLS {
            if self.can.str.read().bits() & STR_MODE_MASK == mode.str_bits() {
                return Ok(());
            }
        }
        Err(CanError::ModeSwitch)
    }

    /// Non-blocking receive: `Ok(None)` when the RX FIFO is empty, one
    /// frame per call otherwise — the caller's poll ticker drains the
    /// 4-deep FIFO by looping until `Ok(None)` (docs/plan.md §4).
    /// `Err(BusOff)` means the driver already re-initialized itself (R31).
    pub fn try_receive(&mut self) -> Result<Option<Frame>, CanError> {
        // SPEC §5.2 RX path, in order: BOST → RFEST → MB28 window →
        // RFPCR 0xFF → RFMLF clear.
        if self.can.str.read().bost().is_1() {
            return Err(self.recover_bus_off());
        }
        if self.can.rfcr.read().rfest().is_1() {
            return Ok(None); // FIFO empty
        }

        // MB[28] is the RX FIFO access window. The FIDCR filter is exact,
        // but mask the SID read anyway (C parity).
        let sid = self.can.mb28_id.read().sid().bits() & SID_MASK;
        // Masked to 11 bits above, so `new` cannot return `None`.
        let id = StandardId::new(sid).unwrap_or(StandardId::ZERO);

        // DLC > 8 is legal on the wire — clamp (C parity).
        let dlc = self.can.mb28_dl.read().dlc().bits().min(FRAME_DATA_MAX);

        // Divergence from C (which copied exactly `dlc` bytes): the PAC
        // names each data-byte register individually, so read all 8 —
        // mailbox-RAM reads have no side effects — then zero the tail to
        // keep Frame's canonical-form invariant.
        let mut data = [
            self.can.mb28_d0.read().bits(),
            self.can.mb28_d1.read().bits(),
            self.can.mb28_d2.read().bits(),
            self.can.mb28_d3.read().bits(),
            self.can.mb28_d4.read().bits(),
            self.can.mb28_d5.read().bits(),
            self.can.mb28_d6.read().bits(),
            self.can.mb28_d7.read().bits(),
        ];
        for stale in &mut data[usize::from(dlc)..] {
            *stale = 0;
        }

        // Advance the CPU-side FIFO pointer.
        // SAFETY: 0xFF is the documented RFPCR advance value (SPEC §5.2).
        self.can.rfpcr.write(|w| unsafe { w.bits(RFPCR_ADVANCE) });

        // Message lost (MLM = 0 overwrite mode): clear RFMLF, keep RFE
        // (C parity: rewrite `RFCR = RFE`).
        if self.can.rfcr.read().rfmlf().is_1() {
            // SAFETY: 0x01 = RFE set, RFMLF clear (SPEC §5.2).
            self.can.rfcr.write(|w| unsafe { w.bits(RFCR_RFE) });
        }

        Ok(Some(Frame { id, dlc, data }))
    }

    /// Transmit one frame from mailbox 0 and poll SENTDATA to completion,
    /// bounded to ~200 ms worst case (cycle-counted — no `embassy-time`;
    /// see `TX_TIMEOUT_POLLS`). On `Err(TxTimeout)` the request was
    /// aborted; on `Err(BusOff)` the driver already re-initialized (R31).
    ///
    /// The C driver validated id/dlc/data here; [`Frame`]'s invariants
    /// make bad arguments unrepresentable instead.
    pub fn send(&mut self, frame: &Frame) -> Result<(), CanError> {
        if self.can.str.read().bost().is_1() {
            return Err(self.recover_bus_off());
        }

        // SPEC §5.2 TX path: load MB[0] with IDE = 0, RTR = 0 (data frame,
        // standard ID — C parity).
        self.can.mb0_id.write(|w| {
            // SAFETY: SID is an 11-bit field; StandardId ≤ 0x7FF by type.
            unsafe { w.sid().bits(frame.id.as_raw()) }
                .ide()
                ._0()
                .rtr()
                ._0()
        });
        // SAFETY: DLC is a 4-bit field; Frame guarantees dlc ≤ 8.
        self.can
            .mb0_dl
            .write(|w| unsafe { w.dlc().bits(frame.dlc) });
        // Divergence from C (which wrote exactly `dlc` bytes): the data
        // registers are individually named, so write all 8 — bytes past
        // DLC never reach the wire, and Frame zero-pads them.
        // SAFETY: whole-byte payload registers, plain data.
        unsafe {
            self.can.mb0_d0.write(|w| w.bits(frame.data[0]));
            self.can.mb0_d1.write(|w| w.bits(frame.data[1]));
            self.can.mb0_d2.write(|w| w.bits(frame.data[2]));
            self.can.mb0_d3.write(|w| w.bits(frame.data[3]));
            self.can.mb0_d4.write(|w| w.bits(frame.data[4]));
            self.can.mb0_d5.write(|w| w.bits(frame.data[5]));
            self.can.mb0_d6.write(|w| w.bits(frame.data[6]));
            self.can.mb0_d7.write(|w| w.bits(frame.data[7]));
        }

        // Request transmit (TRMREQ), then poll SENTDATA (SPEC §5.2).
        self.can.mctl_tx()[TX_MAILBOX].write(|w| w.trmreq()._1());

        for _ in 0..TX_TIMEOUT_POLLS {
            if self.can.mctl_tx()[TX_MAILBOX].read().sentdata().is_1() {
                self.clear_tx_mctl();
                return Ok(());
            }
            if self.can.str.read().bost().is_1() {
                return Err(self.recover_bus_off());
            }
        }

        // Timeout: abort the pending request (same double clear).
        self.clear_tx_mctl();
        Err(CanError::TxTimeout)
    }

    /// SENTDATA and TRMREQ cannot be cleared in one register write — write
    /// 0x00 twice (SPEC §9.1.3, the MCTL double-clear gotcha; FSP's
    /// `r_can.c` does the same).
    fn clear_tx_mctl(&self) {
        // SAFETY: whole-register 0x00, twice by hardware requirement.
        self.can.mctl_tx()[TX_MAILBOX].write(|w| unsafe { w.bits(0) });
        self.can.mctl_tx()[TX_MAILBOX].write(|w| unsafe { w.bits(0) });
    }

    /// True while the controller is bus-off (`STR.BOST`). `try_receive`/
    /// `send` already self-recover (R31); this is for callers that want to
    /// observe the condition without touching the bus.
    #[allow(dead_code)] // diagnostic observer; v1's task relies on the self-recovering paths
    pub fn is_bus_off(&self) -> bool {
        self.can.str.read().bost().is_1()
    }

    /// Full re-initialization with the stored bitrate/filter (R31).
    /// Entering reset mode clears the latched bus-off condition and the
    /// error counters; fails with [`CanError::ModeSwitch`] like `init`.
    pub fn reinit(&mut self) -> Result<(), CanError> {
        self.init_hw()
    }

    /// Stop CAN0 before the jump-to-app / GO reset (R32): reset mode
    /// (result deliberately ignored — the module is being stopped
    /// regardless, C parity), module-stop re-asserted with a completion
    /// read-back, and P102/P103 PmnPFS restored to their 0x0000_0000 reset
    /// state (GPIO input, no pull).
    pub fn deinit(self) {
        let _ = self.set_mode(CanMode::Reset); // intentional: stopping regardless

        // SAFETY: same transient singleton re-acquisition as `init_hw`,
        // under the same no-concurrent-user invariants.
        let p = unsafe { ra4m1::Peripherals::steal() };
        p.MSTP.mstpcrb.modify(|_, w| w.mstpb2()._1());
        let _ = p.MSTP.mstpcrb.read();

        pfs::reset_to_gpio_input(&p.PFS, &p.PMISC, CAN_TX_PORT, CAN_TX_PIN);
        pfs::reset_to_gpio_input(&p.PFS, &p.PMISC, CAN_RX_PORT, CAN_RX_PIN);
    }

    /// R31 bus-off recovery: full re-init (a failed re-init is deliberately
    /// ignored, C parity — BusOff is the answer either way and the caller
    /// keeps waiting), then report [`CanError::BusOff`].
    fn recover_bus_off(&mut self) -> CanError {
        let _ = self.reinit(); // intentional: see above
        CanError::BusOff
    }
}
