//! CAN Update Protocol v1 engine — phases, windowing, replies (SPEC §4,
//! R16–R26; SIZE/ERASE/HELLO values superseded by docs/plan.md §1.2).
//!
//! The engine is a pure state machine: the firmware task feeds it [`Event`]s
//! (received frames plus completions of previously requested operations) and
//! gets back an [`Output`] — at most one reply frame to transmit on the
//! effective boot→host ID and at most one abstract [`Effect`] to perform.
//! Flash, CAN, and the clock never appear here, so every wire behavior is
//! host-testable (R41).
//!
//! # Effect / completion contract
//!
//! Fallible or slow operations are split across two events:
//!
//! | Effect emitted            | Completion the task must feed back |
//! |---------------------------|------------------------------------|
//! | [`Effect::EraseAll`]      | [`Event::EraseDone`]               |
//! | [`Effect::ProgramBlock`]  | [`Event::ProgramDone`]             |
//! | [`Effect::ComputeCrc`]    | [`Event::CrcResult`]               |
//! | [`Effect::WriteMetadata`] | [`Event::MetadataDone`]            |
//!
//! The reply frame the host is waiting for is produced by the *completion*
//! event, matching the C engine where the flash call ran inline. The task
//! performs each effect synchronously before draining more frames; should a
//! frame slip in while an effect is outstanding anyway, the engine NAKs
//! `ERR_STATE` (HELLO still restarts, INFO is still served) rather than
//! corrupt the in-flight operation, and a completion that matches no
//! outstanding effect is ignored — the machine can never wedge.
//!
//! # CRC seam (VERIFY)
//!
//! The engine never sees flash contents, so VERIFY emits
//! [`Effect::ComputeCrc`] naming how many bytes to hash from the app base;
//! the task streams flash through the CRC-32 and answers with
//! [`Event::CrcResult`]. The comparison against the host's expected value
//! stays *inside* the engine — the task computes, the engine decides — so the
//! commit decision (metadata write only on match, SPEC §4.6) is host-tested.

use crate::dfu_config::NAME_LEN;

/// Reply-frame marker, byte 0 of every ACK/NAK/INFO frame (SPEC §4.3).
pub const REPLY_MARKER: u8 = 0x79;
/// Protocol version spoken and announced (SPEC §4.2).
pub const PROTOCOL_VERSION: u8 = 0x01;
/// Fill value for unused payload bytes — DLC is always 8 (SPEC §4.1).
pub const FILL: u8 = 0xFF;

/// HELLO — restarts the session from any phase (SPEC §4.2).
pub const CMD_HELLO: u8 = 0x01;
/// ERASE — accepted in SIZED only.
pub const CMD_ERASE: u8 = 0x02;
/// DATA — accepted in TRANSFER only.
pub const CMD_DATA: u8 = 0x03;
/// SIZE — accepted in GREETED only.
pub const CMD_SIZE: u8 = 0x04;
/// VERIFY — accepted in TRANSFERRED only.
pub const CMD_VERIFY: u8 = 0x05;
/// GO — accepted in VERIFIED only.
pub const CMD_GO: u8 = 0x06;
/// INFO — any time, session state untouched (SPEC §4.7).
pub const CMD_INFO: u8 = 0x07;

/// Unexpected DATA seq; NAK arg = window-start seq to rewind to.
pub const ERR_SEQ: u8 = 0x01;
/// Flash program/erase/read-back failure.
pub const ERR_FLASH: u8 = 0x02;
/// VERIFY CRC mismatch — metadata NOT written, fail closed.
pub const ERR_CRC: u8 = 0x03;
/// SIZE zero or above the app-slot capacity.
pub const ERR_SIZE: u8 = 0x04;
/// Out-of-order/unknown command, bad HELLO version, or DLC ≠ 8.
pub const ERR_STATE: u8 = 0x05;

/// Image payload bytes carried per DATA frame (bytes 2..8, SPEC §4.4).
pub const DATA_BYTES_PER_FRAME: usize = 6;
/// Frames in the INFO reply: 1 header + 4 name frames (SPEC §4.7).
pub const INFO_REPLY_FRAMES: usize = 5;

/// App-slot capacity in bytes: 111 × 2 KB (docs/plan.md §1.2, supersedes R20).
pub(crate) use crate::memory_map::APP_SLOT_SIZE;
/// Flash erase-block size = one protocol window (SPEC §4.4).
pub(crate) use crate::memory_map::FLASH_BLOCK_SIZE as BLOCK_SIZE;
/// Code-flash write unit; the final short block pads to a multiple of this.
pub(crate) const WRITE_UNIT: usize = crate::memory_map::FLASH_WRITE_UNIT as usize;

/// Session phase as visible on the wire (SPEC §4.2 `can_update_phase_t`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Phase {
    /// Power-on state; only HELLO (and INFO) get a positive response.
    Idle,
    /// HELLO accepted; awaiting SIZE.
    Greeted,
    /// SIZE accepted; awaiting ERASE.
    Sized,
    /// ERASE completed; DATA windows in flight.
    Transfer,
    /// Full image received; awaiting VERIFY.
    Transferred,
    /// Metadata committed; awaiting GO.
    Verified,
}

/// Operation requested of the task and not yet completed (module doc table).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum PendingOp {
    Erase,
    Program,
    Crc,
    Metadata,
}

/// Abstract side effect for the firmware task to perform. Flash geometry
/// (addresses) is the task's business; the engine speaks block indexes.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Effect {
    /// Erase the metadata block FIRST, then every app-slot block — the
    /// fail-closed ordering of SPEC §4.5. Answer with [`Event::EraseDone`].
    EraseAll,
    /// Program [`Engine::window`]'s bytes at app-slot block `index`
    /// (0..=110; address = APP_BASE + index × 2048) and read-back verify.
    /// Answer with [`Event::ProgramDone`].
    ProgramBlock {
        /// App-slot block index, 0..=110.
        index: u32,
    },
    /// Compute CRC-32 over flash `[APP_BASE, APP_BASE + len)` and answer
    /// with [`Event::CrcResult`]. `len` is the SIZE-declared image size.
    ComputeCrc {
        /// Bytes to hash, 1..=`APP_SLOT_SIZE`.
        len: u32,
    },
    /// CRC matched: write the 16-byte metadata record — the commit point
    /// (SPEC §4.6). Answer with [`Event::MetadataDone`].
    WriteMetadata {
        /// Image size in bytes for the record.
        image_size: u32,
        /// CRC-32 of the image for the record.
        image_crc: u32,
    },
    /// Serve the 5-frame INFO reply ([`build_info_reply`]); no completion
    /// event — session state is untouched (SPEC §4.7).
    SendInfo,
    /// GO accepted: transmit the ACK in [`Output::reply`] FIRST, then deinit
    /// CAN and `NVIC_SystemReset()` (SPEC §4.6, R13). No completion event.
    Reset,
}

/// Input to the engine: a received frame or the completion of an [`Effect`].
#[derive(Debug, Clone, Copy)]
pub enum Event<'a> {
    /// A received CAN frame: 11-bit `id` and its payload (`data.len()` =
    /// DLC). Foreign IDs are ignored; DLC ≠ 8 NAKs `ERR_STATE` (SPEC §4.3).
    Frame {
        /// Received 11-bit CAN ID.
        id: u16,
        /// Frame payload; length is the received DLC.
        data: &'a [u8],
    },
    /// [`Effect::EraseAll`] finished; `true` = every block erased.
    EraseDone(bool),
    /// [`Effect::ProgramBlock`] finished; `true` = programmed and verified.
    ProgramDone(bool),
    /// [`Effect::ComputeCrc`] result.
    CrcResult(u32),
    /// [`Effect::WriteMetadata`] finished; `true` = record committed.
    MetadataDone(bool),
}

/// What one [`Engine::handle`] call asks of the task: transmit `reply` (if
/// any) on the boot→host ID, then perform `effect` (if any).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Output {
    /// Reply frame payload (always DLC 8) or `None` — mid-window DATA frames
    /// and foreign-ID frames produce no reply (SPEC §4.4, §4.3).
    pub reply: Option<[u8; 8]>,
    /// Side effect to perform, or `None`.
    pub effect: Option<Effect>,
}

impl Output {
    const fn none() -> Self {
        Self {
            reply: None,
            effect: None,
        }
    }

    const fn reply(frame: [u8; 8]) -> Self {
        Self {
            reply: Some(frame),
            effect: None,
        }
    }

    const fn effect(effect: Effect) -> Self {
        Self {
            reply: None,
            effect: Some(effect),
        }
    }
}

/// The protocol engine. Owns the 2 KB window buffer, so one instance costs
/// ~2 KB of RAM — the task allocates it once (e.g. via `StaticCell`).
pub struct Engine {
    /// Effective host→boot ID (default `0x79E` or v2-config override);
    /// frames on any other ID are silently ignored.
    cmd_id: u16,
    phase: Phase,
    pending: Option<PendingOp>,
    /// SIZE-declared image size in bytes; valid from `Sized` onward.
    image_size: u32,
    /// Host's expected image CRC from VERIFY.
    expected_crc: u32,
    /// Bytes successfully programmed (whole blocks); rewinds never cross it.
    committed: u32,
    /// Bytes accumulated in the current window buffer.
    window_fill: usize,
    /// Seq the next DATA frame must carry.
    expected_seq: u8,
    /// Seq of the current window's first frame — the `ERR_SEQ` rewind target.
    window_start_seq: u8,
    window: [u8; BLOCK_SIZE as usize],
}

impl Engine {
    /// New engine matching frames addressed to `cmd_id` (11-bit; the
    /// effective host→boot ID from [`crate::dfu_config`]). Starts in
    /// [`Phase::Idle`].
    pub fn new(cmd_id: u16) -> Self {
        Self {
            cmd_id,
            phase: Phase::Idle,
            pending: None,
            image_size: 0,
            expected_crc: 0,
            committed: 0,
            window_fill: 0,
            expected_seq: 0,
            window_start_seq: 0,
            window: [FILL; BLOCK_SIZE as usize],
        }
    }

    /// Current wire-visible phase (for logging/tests).
    pub fn phase(&self) -> Phase {
        self.phase
    }

    /// Bytes to program for the outstanding [`Effect::ProgramBlock`]: the
    /// window contents tail-padded with `0xFF` to a whole number of 8-byte
    /// write units (SPEC §4.4). Meaningful only between a `ProgramBlock`
    /// emission and its [`Event::ProgramDone`]; 8..=2048 bytes long there.
    pub fn window(&self) -> &[u8] {
        &self.window[..self.window_fill.next_multiple_of(WRITE_UNIT)]
    }

    /// Advance the machine by one event. Never panics on any input; unknown
    /// or ill-timed inputs produce an `ERR_STATE` NAK or are ignored per
    /// SPEC §4.3's frame-handling rules.
    pub fn handle(&mut self, event: Event<'_>) -> Output {
        match event {
            Event::Frame { id, data } => self.on_frame(id, data),
            Event::EraseDone(ok) => self.on_erase_done(ok),
            Event::ProgramDone(ok) => self.on_program_done(ok),
            Event::CrcResult(crc) => self.on_crc_result(crc),
            Event::MetadataDone(ok) => self.on_metadata_done(ok),
        }
    }

    /// Convenience wrapper for the common case: `handle(Event::Frame {..})`.
    pub fn handle_frame(&mut self, id: u16, data: &[u8]) -> Output {
        self.handle(Event::Frame { id, data })
    }

    fn on_frame(&mut self, id: u16, data: &[u8]) -> Output {
        // Foreign IDs: silence — not our conversation (SPEC §4.3).
        if id != self.cmd_id {
            return Output::none();
        }
        let Ok(frame) = <&[u8; 8]>::try_from(data) else {
            // DLC ≠ 8 on our ID: NAK ERR_STATE echoing byte0 (SPEC §4.3).
            let echo = data.first().copied().unwrap_or(0x00);
            return Output::reply(nak(echo, ERR_STATE, 0));
        };
        let cmd = frame[0];
        if cmd == CMD_INFO {
            return Output::effect(Effect::SendInfo);
        }
        if cmd == CMD_HELLO {
            return self.on_hello(frame);
        }
        if self.pending.is_some() {
            // Unreachable when the task completes effects before draining
            // more frames; guarded so misuse can't corrupt the in-flight op.
            return Output::reply(nak(cmd, ERR_STATE, 0));
        }
        match (cmd, self.phase) {
            (CMD_SIZE, Phase::Greeted) => self.on_size(frame),
            (CMD_ERASE, Phase::Sized) => {
                self.pending = Some(PendingOp::Erase);
                Output::effect(Effect::EraseAll)
            }
            (CMD_DATA, Phase::Transfer) => self.on_data(frame),
            (CMD_VERIFY, Phase::Transferred) => {
                self.expected_crc = le_u32(frame, 2);
                self.pending = Some(PendingOp::Crc);
                Output::effect(Effect::ComputeCrc {
                    len: self.image_size,
                })
            }
            (CMD_GO, Phase::Verified) => Output {
                reply: Some(ack(CMD_GO, 0)),
                effect: Some(Effect::Reset),
            },
            _ => Output::reply(nak(cmd, ERR_STATE, 0)),
        }
    }

    fn on_hello(&mut self, frame: &[u8; 8]) -> Output {
        if frame[1] != PROTOCOL_VERSION {
            return Output::reply(nak(CMD_HELLO, ERR_STATE, 0));
        }
        // Session restart from ANY phase, including mid-window and while an
        // effect is outstanding (SPEC §4.2); a stale completion arriving
        // afterwards no longer matches `pending` and is ignored.
        self.phase = Phase::Greeted;
        self.pending = None;
        self.image_size = 0;
        self.expected_crc = 0;
        self.committed = 0;
        self.window_fill = 0;
        self.expected_seq = 0;
        self.window_start_seq = 0;
        let slot = APP_SLOT_SIZE.to_le_bytes();
        Output::reply([
            REPLY_MARKER,
            CMD_HELLO,
            0x00,
            PROTOCOL_VERSION,
            slot[0],
            slot[1],
            slot[2],
            slot[3],
        ])
    }

    fn on_size(&mut self, frame: &[u8; 8]) -> Output {
        let size = le_u32(frame, 2);
        if size == 0 || size > APP_SLOT_SIZE {
            return Output::reply(nak(CMD_SIZE, ERR_SIZE, 0));
        }
        self.image_size = size;
        self.phase = Phase::Sized;
        Output::reply(ack(CMD_SIZE, 0))
    }

    fn on_data(&mut self, frame: &[u8; 8]) -> Output {
        let seq = frame[1];
        if seq != self.expected_seq {
            // Rewind: discard the window (completed blocks stay counted) and
            // NAK every frame of the stale burst (SPEC §4.4).
            self.discard_window();
            return Output::reply(nak(CMD_DATA, ERR_SEQ, self.window_start_seq));
        }
        // Clamp against both the block boundary and image_size regardless of
        // how the host packed the frame (SPEC §4.4).
        let target = self.window_target();
        let take = DATA_BYTES_PER_FRAME.min(target - self.window_fill);
        self.window[self.window_fill..self.window_fill + take].copy_from_slice(&frame[2..2 + take]);
        self.window_fill += take;
        self.expected_seq = self.expected_seq.wrapping_add(1);
        if self.window_fill < target {
            // Mid-window frames get NO reply (SPEC §4.4).
            return Output::none();
        }
        // Block complete: pad the tail to a write-unit boundary and hand it
        // to the task; the window ACK follows ProgramDone(true).
        let padded = target.next_multiple_of(WRITE_UNIT);
        self.window[target..padded].fill(FILL);
        self.pending = Some(PendingOp::Program);
        Output::effect(Effect::ProgramBlock {
            index: self.committed / BLOCK_SIZE,
        })
    }

    fn on_erase_done(&mut self, ok: bool) -> Output {
        if self.pending != Some(PendingOp::Erase) {
            return Output::none();
        }
        self.pending = None;
        if !ok {
            // Stay in Sized: the host may retry ERASE after its own recovery.
            return Output::reply(nak(CMD_ERASE, ERR_FLASH, 0));
        }
        self.phase = Phase::Transfer;
        self.committed = 0;
        self.window_fill = 0;
        self.expected_seq = 0;
        self.window_start_seq = 0;
        Output::reply(ack(CMD_ERASE, 0))
    }

    fn on_program_done(&mut self, ok: bool) -> Output {
        if self.pending != Some(PendingOp::Program) {
            return Output::none();
        }
        self.pending = None;
        if !ok {
            // Window discarded, phase stays Transfer; host resends the
            // window from window_start_seq (SPEC §4.4).
            self.discard_window();
            return Output::reply(nak(CMD_DATA, ERR_FLASH, self.window_start_seq));
        }
        // window_target() is still this window's size: committed moves after.
        let programmed = self.window_target();
        // Lossless: programmed ≤ BLOCK_SIZE (2048).
        self.committed += programmed as u32;
        self.window_fill = 0;
        self.window_start_seq = self.expected_seq;
        if self.committed == self.image_size {
            self.phase = Phase::Transferred;
        }
        Output::reply(ack(CMD_DATA, self.expected_seq.wrapping_sub(1)))
    }

    fn on_crc_result(&mut self, crc: u32) -> Output {
        if self.pending != Some(PendingOp::Crc) {
            return Output::none();
        }
        self.pending = None;
        if crc != self.expected_crc {
            // Fail closed: metadata stays erased (SPEC §4.6). Host may
            // re-VERIFY (it won't — a NAK here is fatal to it — but the
            // engine stays consistent).
            return Output::reply(nak(CMD_VERIFY, ERR_CRC, 0));
        }
        self.pending = Some(PendingOp::Metadata);
        Output::effect(Effect::WriteMetadata {
            image_size: self.image_size,
            image_crc: crc,
        })
    }

    fn on_metadata_done(&mut self, ok: bool) -> Output {
        if self.pending != Some(PendingOp::Metadata) {
            return Output::none();
        }
        self.pending = None;
        if !ok {
            // Metadata possibly torn — still fail closed (header CRC will
            // reject it at boot); stay Transferred so VERIFY can be retried.
            return Output::reply(nak(CMD_VERIFY, ERR_FLASH, 0));
        }
        self.phase = Phase::Verified;
        Output::reply(ack(CMD_VERIFY, 0))
    }

    /// Size of the window currently being filled: a full block or the final
    /// short remainder of the image (SPEC §4.4).
    fn window_target(&self) -> usize {
        // Lossless: the min is ≤ BLOCK_SIZE (2048).
        BLOCK_SIZE.min(self.image_size.saturating_sub(self.committed)) as usize
    }

    fn discard_window(&mut self) {
        self.window_fill = 0;
        self.expected_seq = self.window_start_seq;
    }
}

/// Build the 5-frame INFO reply (SPEC §4.7): header frame + the 32-byte
/// product-name field, 8 bytes per frame, ASCII, `0x00`-padded past the
/// name. `name` longer than 32 bytes is truncated to 32. Pass an empty name
/// when the config block is absent/invalid or data flash is unproven (R25).
pub fn build_info_reply(name: &[u8]) -> [[u8; 8]; INFO_REPLY_FRAMES] {
    let len = name.len().min(NAME_LEN);
    let mut frames = [[0x00; 8]; INFO_REPLY_FRAMES];
    // Lossless: len ≤ NAME_LEN (32).
    frames[0] = [
        REPLY_MARKER,
        CMD_INFO,
        len as u8,
        PROTOCOL_VERSION,
        FILL,
        FILL,
        FILL,
        FILL,
    ];
    for (i, &byte) in name.iter().take(len).enumerate() {
        frames[1 + i / 8][i % 8] = byte;
    }
    frames
}

/// `{0x79, cmd_echo, 0x00, arg, FF, FF, FF, FF}` (SPEC §4.3).
fn ack(cmd_echo: u8, arg: u8) -> [u8; 8] {
    [REPLY_MARKER, cmd_echo, 0x00, arg, FILL, FILL, FILL, FILL]
}

/// `{0x79, cmd_echo, err, arg, FF, FF, FF, FF}` (SPEC §4.3).
fn nak(cmd_echo: u8, err: u8, arg: u8) -> [u8; 8] {
    [REPLY_MARKER, cmd_echo, err, arg, FILL, FILL, FILL, FILL]
}

fn le_u32(frame: &[u8; 8], offset: usize) -> u32 {
    u32::from_le_bytes([
        frame[offset],
        frame[offset + 1],
        frame[offset + 2],
        frame[offset + 3],
    ])
}

#[cfg(test)]
mod tests {
    use super::*;

    const CMD_ID: u16 = 0x79E;
    const F: u8 = FILL;

    fn engine() -> Engine {
        Engine::new(CMD_ID)
    }

    fn hello_frame() -> [u8; 8] {
        [CMD_HELLO, PROTOCOL_VERSION, F, F, F, F, F, F]
    }

    fn size_frame(size: u32) -> [u8; 8] {
        let s = size.to_le_bytes();
        [CMD_SIZE, F, s[0], s[1], s[2], s[3], F, F]
    }

    fn erase_frame() -> [u8; 8] {
        [CMD_ERASE, F, F, F, F, F, F, F]
    }

    fn data_frame(seq: u8, payload: &[u8]) -> [u8; 8] {
        let mut f = [CMD_DATA, seq, F, F, F, F, F, F];
        f[2..2 + payload.len()].copy_from_slice(payload);
        f
    }

    fn verify_frame(crc: u32) -> [u8; 8] {
        let c = crc.to_le_bytes();
        [CMD_VERIFY, F, c[0], c[1], c[2], c[3], F, F]
    }

    fn go_frame() -> [u8; 8] {
        [CMD_GO, F, F, F, F, F, F, F]
    }

    fn ack_bytes(cmd: u8, arg: u8) -> [u8; 8] {
        [0x79, cmd, 0x00, arg, F, F, F, F]
    }

    fn nak_bytes(cmd: u8, err: u8, arg: u8) -> [u8; 8] {
        [0x79, cmd, err, arg, F, F, F, F]
    }

    /// Deterministic test image contents.
    fn image_byte(offset: usize) -> u8 {
        (offset.wrapping_mul(31).wrapping_add(7) % 251) as u8
    }

    fn hello(e: &mut Engine) -> Output {
        e.handle_frame(CMD_ID, &hello_frame())
    }

    /// Drive HELLO → SIZE → ERASE(+done) into Transfer.
    fn enter_transfer(e: &mut Engine, size: u32) {
        assert_eq!(hello(e).reply, Some(hello_ack_bytes()));
        assert_eq!(
            e.handle_frame(CMD_ID, &size_frame(size)).reply,
            Some(ack_bytes(CMD_SIZE, 0))
        );
        let out = e.handle_frame(CMD_ID, &erase_frame());
        assert_eq!(out.effect, Some(Effect::EraseAll));
        assert_eq!(out.reply, None);
        assert_eq!(
            e.handle(Event::EraseDone(true)).reply,
            Some(ack_bytes(CMD_ERASE, 0))
        );
        assert_eq!(e.phase(), Phase::Transfer);
    }

    /// Feed one whole window's DATA frames starting at `image[offset..]`,
    /// with `start_seq`, returning the Output of the block-completing frame.
    fn send_window(e: &mut Engine, image: &[u8], offset: usize, start_seq: u8) -> Output {
        let target = (image.len() - offset).min(BLOCK_SIZE as usize);
        let mut seq = start_seq;
        let mut sent = 0usize;
        loop {
            let n = DATA_BYTES_PER_FRAME.min(target - sent);
            let out = e.handle_frame(
                CMD_ID,
                &data_frame(seq, &image[offset + sent..offset + sent + n]),
            );
            sent += n;
            seq = seq.wrapping_add(1);
            if sent == target {
                return out;
            }
            assert_eq!(out, Output::none(), "mid-window frame must be silent");
        }
    }

    /// Full transfer of `image`, checking window ACK args; leaves Transferred.
    fn transfer_image(e: &mut Engine, image: &[u8]) {
        let mut offset = 0usize;
        let mut seq = 0u8;
        while offset < image.len() {
            let target = (image.len() - offset).min(BLOCK_SIZE as usize);
            let frames = target.div_ceil(DATA_BYTES_PER_FRAME);
            let out = send_window(e, image, offset, seq);
            assert_eq!(
                out.effect,
                Some(Effect::ProgramBlock {
                    index: (offset as u32) / BLOCK_SIZE
                })
            );
            assert_eq!(&e.window()[..target], &image[offset..offset + target]);
            seq = seq.wrapping_add(frames as u8);
            let ack = e.handle(Event::ProgramDone(true));
            assert_eq!(ack.reply, Some(ack_bytes(CMD_DATA, seq.wrapping_sub(1))));
            offset += target;
        }
        assert_eq!(e.phase(), Phase::Transferred);
    }

    fn hello_ack_bytes() -> [u8; 8] {
        // APP_SLOT_SIZE 0x0003_7800 LE = 00 78 03 00 (docs/plan.md §1.2).
        [0x79, 0x01, 0x00, 0x01, 0x00, 0x78, 0x03, 0x00]
    }

    fn make_image(len: usize) -> Vec<u8> {
        (0..len).map(image_byte).collect()
    }

    // ---- HELLO ----

    #[test]
    fn hello_ack_is_byte_exact_and_announces_slot_size() {
        let mut e = engine();
        let out = hello(&mut e);
        assert_eq!(out.reply, Some(hello_ack_bytes()));
        assert_eq!(out.effect, None);
        assert_eq!(e.phase(), Phase::Greeted);
    }

    #[test]
    fn hello_bad_version_naks_err_state_and_keeps_phase() {
        let mut e = engine();
        for bad in [0x00, 0x02, 0xFF] {
            let out = e.handle_frame(CMD_ID, &[CMD_HELLO, bad, F, F, F, F, F, F]);
            assert_eq!(out.reply, Some(nak_bytes(CMD_HELLO, ERR_STATE, 0)));
            assert_eq!(e.phase(), Phase::Idle);
        }
    }

    #[test]
    fn hello_restarts_from_every_phase() {
        // Idle
        let mut e = engine();
        assert_eq!(hello(&mut e).reply, Some(hello_ack_bytes()));

        // Greeted
        assert_eq!(hello(&mut e).reply, Some(hello_ack_bytes()));
        assert_eq!(e.phase(), Phase::Greeted);

        // Sized
        e.handle_frame(CMD_ID, &size_frame(100));
        assert_eq!(e.phase(), Phase::Sized);
        assert_eq!(hello(&mut e).reply, Some(hello_ack_bytes()));
        assert_eq!(e.phase(), Phase::Greeted);

        // Transfer, mid-window
        let mut e = engine();
        enter_transfer(&mut e, 100);
        e.handle_frame(CMD_ID, &data_frame(0, &[1, 2, 3, 4, 5, 6]));
        assert_eq!(hello(&mut e).reply, Some(hello_ack_bytes()));
        assert_eq!(e.phase(), Phase::Greeted);

        // Transferred
        let img = make_image(64);
        let mut e = engine();
        enter_transfer(&mut e, 64);
        transfer_image(&mut e, &img);
        assert_eq!(hello(&mut e).reply, Some(hello_ack_bytes()));
        assert_eq!(e.phase(), Phase::Greeted);

        // Verified
        let mut e = engine();
        enter_transfer(&mut e, 64);
        transfer_image(&mut e, &img);
        e.handle_frame(CMD_ID, &verify_frame(0xDEAD_BEEF));
        e.handle(Event::CrcResult(0xDEAD_BEEF));
        e.handle(Event::MetadataDone(true));
        assert_eq!(e.phase(), Phase::Verified);
        assert_eq!(hello(&mut e).reply, Some(hello_ack_bytes()));
        assert_eq!(e.phase(), Phase::Greeted);
    }

    #[test]
    fn hello_during_pending_restarts_and_stale_completion_is_ignored() {
        let mut e = engine();
        enter_transfer(&mut e, 8);
        let out = send_window(&mut e, &make_image(8), 0, 0);
        assert_eq!(out.effect, Some(Effect::ProgramBlock { index: 0 }));
        // Restart while the program effect is outstanding.
        assert_eq!(hello(&mut e).reply, Some(hello_ack_bytes()));
        // The stale completion must not produce a reply or corrupt state.
        assert_eq!(e.handle(Event::ProgramDone(true)), Output::none());
        assert_eq!(e.phase(), Phase::Greeted);
    }

    // ---- ID / DLC filtering ----

    #[test]
    fn foreign_id_frames_are_silently_ignored_in_every_phase() {
        let mut e = engine();
        for foreign in [0x000u16, 0x79F, 0x7FF] {
            assert_eq!(e.handle_frame(foreign, &hello_frame()), Output::none());
        }
        assert_eq!(e.phase(), Phase::Idle);
        enter_transfer(&mut e, 100);
        assert_eq!(
            e.handle_frame(0x79F, &data_frame(0, &[1, 2, 3, 4, 5, 6])),
            Output::none()
        );
        assert_eq!(e.phase(), Phase::Transfer);
    }

    #[test]
    fn dlc_not_8_naks_err_state_echoing_byte0() {
        let mut e = engine();
        let out = e.handle_frame(CMD_ID, &[CMD_HELLO, PROTOCOL_VERSION]);
        assert_eq!(out.reply, Some(nak_bytes(CMD_HELLO, ERR_STATE, 0)));
        // Empty payload echoes 0x00.
        let out = e.handle_frame(CMD_ID, &[]);
        assert_eq!(out.reply, Some(nak_bytes(0x00, ERR_STATE, 0)));
        // 7 and 9 bytes are equally rejected.
        let seven = [CMD_DATA, 0, 1, 2, 3, 4, 5];
        for data in [&seven[..], &[0x03u8; 9][..]] {
            let out = e.handle_frame(CMD_ID, data);
            assert_eq!(out.reply, Some(nak_bytes(0x03, ERR_STATE, 0)));
        }
        assert_eq!(e.phase(), Phase::Idle);
    }

    // ---- phase-order enforcement ----

    #[test]
    fn every_command_in_every_wrong_phase_naks_err_state() {
        // (frame, set of phases where it is accepted)
        let cases: [([u8; 8], &[Phase]); 5] = [
            (size_frame(100), &[Phase::Greeted]),
            (erase_frame(), &[Phase::Sized]),
            (data_frame(0, &[0; 6]), &[Phase::Transfer]),
            (verify_frame(0), &[Phase::Transferred]),
            (go_frame(), &[Phase::Verified]),
        ];
        let phases = [
            Phase::Idle,
            Phase::Greeted,
            Phase::Sized,
            Phase::Transfer,
            Phase::Transferred,
            Phase::Verified,
        ];
        for phase in phases {
            for (frame, accepted_in) in &cases {
                if accepted_in.contains(&phase) {
                    continue;
                }
                let mut e = engine_in_phase(phase);
                let out = e.handle_frame(CMD_ID, frame);
                assert_eq!(
                    out.reply,
                    Some(nak_bytes(frame[0], ERR_STATE, 0)),
                    "cmd {:#04x} in phase {:?}",
                    frame[0],
                    phase
                );
                assert_eq!(out.effect, None);
                assert_eq!(e.phase(), phase, "phase must not move on a NAK");
            }
        }
    }

    /// Drive a fresh engine into `phase` via the wire (16-byte image).
    fn engine_in_phase(phase: Phase) -> Engine {
        let mut e = engine();
        if phase == Phase::Idle {
            return e;
        }
        hello(&mut e);
        if phase == Phase::Greeted {
            return e;
        }
        e.handle_frame(CMD_ID, &size_frame(16));
        if phase == Phase::Sized {
            return e;
        }
        e.handle_frame(CMD_ID, &erase_frame());
        e.handle(Event::EraseDone(true));
        if phase == Phase::Transfer {
            return e;
        }
        transfer_image(&mut e, &make_image(16));
        if phase == Phase::Transferred {
            return e;
        }
        e.handle_frame(CMD_ID, &verify_frame(0x1234_5678));
        e.handle(Event::CrcResult(0x1234_5678));
        e.handle(Event::MetadataDone(true));
        assert_eq!(e.phase(), Phase::Verified);
        e
    }

    #[test]
    fn unknown_commands_nak_err_state() {
        let mut e = engine();
        hello(&mut e);
        for cmd in [0x00u8, 0x08, 0x42, 0xFF] {
            let out = e.handle_frame(CMD_ID, &[cmd, F, F, F, F, F, F, F]);
            assert_eq!(out.reply, Some(nak_bytes(cmd, ERR_STATE, 0)));
            assert_eq!(out.effect, None);
        }
        assert_eq!(e.phase(), Phase::Greeted);
    }

    // ---- SIZE bounds ----

    #[test]
    fn size_zero_rejected_with_err_size() {
        let mut e = engine();
        hello(&mut e);
        let out = e.handle_frame(CMD_ID, &size_frame(0));
        assert_eq!(out.reply, Some(nak_bytes(CMD_SIZE, ERR_SIZE, 0)));
        assert_eq!(e.phase(), Phase::Greeted);
    }

    #[test]
    fn size_at_slot_capacity_accepted() {
        let mut e = engine();
        hello(&mut e);
        let out = e.handle_frame(CMD_ID, &size_frame(227_328));
        assert_eq!(out.reply, Some(ack_bytes(CMD_SIZE, 0)));
        assert_eq!(e.phase(), Phase::Sized);
    }

    #[test]
    fn size_above_slot_capacity_rejected() {
        let mut e = engine();
        hello(&mut e);
        for size in [227_329u32, 0x0003_B800, u32::MAX] {
            let out = e.handle_frame(CMD_ID, &size_frame(size));
            assert_eq!(out.reply, Some(nak_bytes(CMD_SIZE, ERR_SIZE, 0)));
            assert_eq!(e.phase(), Phase::Greeted);
        }
    }

    // ---- ERASE ----

    #[test]
    fn erase_failure_naks_err_flash_and_allows_retry() {
        let mut e = engine();
        hello(&mut e);
        e.handle_frame(CMD_ID, &size_frame(100));
        assert_eq!(
            e.handle_frame(CMD_ID, &erase_frame()).effect,
            Some(Effect::EraseAll)
        );
        let out = e.handle(Event::EraseDone(false));
        assert_eq!(out.reply, Some(nak_bytes(CMD_ERASE, ERR_FLASH, 0)));
        assert_eq!(e.phase(), Phase::Sized);
        // Retry succeeds.
        assert_eq!(
            e.handle_frame(CMD_ID, &erase_frame()).effect,
            Some(Effect::EraseAll)
        );
        assert_eq!(
            e.handle(Event::EraseDone(true)).reply,
            Some(ack_bytes(CMD_ERASE, 0))
        );
        assert_eq!(e.phase(), Phase::Transfer);
    }

    // ---- DATA windowing ----

    #[test]
    fn full_block_window_is_342_frames_and_acks_after_program() {
        let img = make_image(2 * BLOCK_SIZE as usize);
        let mut e = engine();
        enter_transfer(&mut e, img.len() as u32);
        let out = send_window(&mut e, &img, 0, 0);
        // 2048 = 341×6 + 2 → 342 frames, seqs 0..=341 (341 % 256 = 85).
        assert_eq!(out.effect, Some(Effect::ProgramBlock { index: 0 }));
        assert_eq!(out.reply, None);
        assert_eq!(e.window(), &img[..2048]);
        let ack = e.handle(Event::ProgramDone(true));
        assert_eq!(ack.reply, Some(ack_bytes(CMD_DATA, 85)));
        assert_eq!(e.phase(), Phase::Transfer);
    }

    #[test]
    fn final_frame_of_full_block_is_clamped_to_2_bytes() {
        // Frame 342 carries 6 payload bytes but only 2 fit the block; the
        // extra 4 must be dropped, not straddled into the next block.
        let img = make_image(2 * BLOCK_SIZE as usize);
        let mut e = engine();
        enter_transfer(&mut e, img.len() as u32);
        let mut seq = 0u8;
        for chunk in img[..2046].chunks(6) {
            assert_eq!(
                e.handle_frame(CMD_ID, &data_frame(seq, chunk)),
                Output::none()
            );
            seq = seq.wrapping_add(1);
        }
        // Block-completing frame stuffed with 6 bytes; the last 4 are garbage.
        let last = [CMD_DATA, seq, img[2046], img[2047], 0xAA, 0xBB, 0xCC, 0xDD];
        let out = e.handle_frame(CMD_ID, &last);
        assert_eq!(out.effect, Some(Effect::ProgramBlock { index: 0 }));
        assert_eq!(
            e.window(),
            &img[..2048],
            "garbage must not enter the window"
        );
    }

    #[test]
    fn short_final_block_pads_to_write_unit_with_0xff() {
        let len = BLOCK_SIZE as usize + 10;
        let img = make_image(len);
        let mut e = engine();
        enter_transfer(&mut e, len as u32);
        transfer_first_block(&mut e, &img);
        // Final window: 10 bytes → frames seq 86 (6 B) and 87 (4 B).
        assert_eq!(
            e.handle_frame(CMD_ID, &data_frame(86, &img[2048..2054])),
            Output::none()
        );
        let out = e.handle_frame(CMD_ID, &data_frame(87, &img[2054..2058]));
        assert_eq!(out.effect, Some(Effect::ProgramBlock { index: 1 }));
        assert_eq!(e.window().len(), 16, "10 bytes pad to two 8-byte units");
        assert_eq!(&e.window()[..10], &img[2048..2058]);
        assert_eq!(&e.window()[10..], &[0xFF; 6]);
        let ack = e.handle(Event::ProgramDone(true));
        assert_eq!(ack.reply, Some(ack_bytes(CMD_DATA, 87)));
        assert_eq!(e.phase(), Phase::Transferred);
    }

    fn transfer_first_block(e: &mut Engine, img: &[u8]) {
        let out = send_window(e, img, 0, 0);
        assert_eq!(out.effect, Some(Effect::ProgramBlock { index: 0 }));
        e.handle(Event::ProgramDone(true));
    }

    #[test]
    fn tiny_image_single_frame_pads_to_8_bytes() {
        let img = make_image(4);
        let mut e = engine();
        enter_transfer(&mut e, 4);
        let out = e.handle_frame(CMD_ID, &data_frame(0, &img));
        assert_eq!(out.effect, Some(Effect::ProgramBlock { index: 0 }));
        assert_eq!(e.window().len(), 8);
        assert_eq!(&e.window()[..4], &img[..]);
        assert_eq!(&e.window()[4..], &[0xFF; 4]);
        e.handle(Event::ProgramDone(true));
        assert_eq!(e.phase(), Phase::Transferred);
    }

    // ---- seq errors & rewind ----

    #[test]
    fn wrong_seq_naks_rewind_target_and_stale_burst_keeps_naking() {
        let img = make_image(2 * BLOCK_SIZE as usize);
        let mut e = engine();
        enter_transfer(&mut e, img.len() as u32);
        transfer_first_block(&mut e, &img);
        // Window 1 starts at seq 86. Send two good frames, then skip one.
        assert_eq!(
            e.handle_frame(CMD_ID, &data_frame(86, &img[2048..2054])),
            Output::none()
        );
        assert_eq!(
            e.handle_frame(CMD_ID, &data_frame(87, &img[2054..2060])),
            Output::none()
        );
        // Frame seq 88 lost; 89 arrives.
        let out = e.handle_frame(CMD_ID, &data_frame(89, &img[2066..2072]));
        assert_eq!(out.reply, Some(nak_bytes(CMD_DATA, ERR_SEQ, 86)));
        // Every further stale frame NAKs the same rewind target.
        for seq in [90u8, 91, 92] {
            let out = e.handle_frame(CMD_ID, &data_frame(seq, &[0; 6]));
            assert_eq!(out.reply, Some(nak_bytes(CMD_DATA, ERR_SEQ, 86)));
        }
        // Retransmission from the window start completes the block; the
        // completed block 0 stayed counted (index is 1).
        let out = send_window(&mut e, &img, 2048, 86);
        assert_eq!(out.effect, Some(Effect::ProgramBlock { index: 1 }));
        assert_eq!(e.window(), &img[2048..4096]);
        let ack = e.handle(Event::ProgramDone(true));
        assert_eq!(ack.reply, Some(ack_bytes(CMD_DATA, 171)));
        assert_eq!(e.phase(), Phase::Transferred);
    }

    #[test]
    fn lost_window_ack_resend_naks_to_next_window_start() {
        // Host resends a window whose ACK was lost; the device must NAK
        // ERR_SEQ pointing at the NEXT window's start seq ("advance", §4.8).
        let img = make_image(2 * BLOCK_SIZE as usize);
        let mut e = engine();
        enter_transfer(&mut e, img.len() as u32);
        transfer_first_block(&mut e, &img); // window 0 committed, ACK "lost"
        let out = e.handle_frame(CMD_ID, &data_frame(0, &img[..6]));
        assert_eq!(out.reply, Some(nak_bytes(CMD_DATA, ERR_SEQ, 86)));
        assert_eq!(e.phase(), Phase::Transfer);
    }

    #[test]
    fn flash_error_discards_window_stays_in_transfer_and_resend_succeeds() {
        let img = make_image(64);
        let mut e = engine();
        enter_transfer(&mut e, 64);
        let out = send_window(&mut e, &img, 0, 0);
        assert_eq!(out.effect, Some(Effect::ProgramBlock { index: 0 }));
        let out = e.handle(Event::ProgramDone(false));
        assert_eq!(out.reply, Some(nak_bytes(CMD_DATA, ERR_FLASH, 0)));
        assert_eq!(e.phase(), Phase::Transfer);
        // Host resends the whole window from seq 0.
        let out = send_window(&mut e, &img, 0, 0);
        assert_eq!(out.effect, Some(Effect::ProgramBlock { index: 0 }));
        assert_eq!(&e.window()[..64], &img[..]);
        let ack = e.handle(Event::ProgramDone(true));
        assert_eq!(ack.reply, Some(ack_bytes(CMD_DATA, 10))); // 11 frames, seqs 0..=10
        assert_eq!(e.phase(), Phase::Transferred);
    }

    // ---- seq wrap over a full 111-block image ----

    #[test]
    fn seq_wrap_geometry_full_111_block_image() {
        let img = make_image(APP_SLOT_SIZE as usize); // 227,328 B = 111 blocks
        let mut e = engine();
        enter_transfer(&mut e, APP_SLOT_SIZE);
        let mut start_seqs = Vec::new();
        let mut seq = 0u8;
        for block in 0..111u32 {
            start_seqs.push(seq);
            // Window-start seq must be 86·k mod 256 (SPEC §4.4).
            assert_eq!(seq, (86u32 * block % 256) as u8, "block {block}");
            let offset = (block * BLOCK_SIZE) as usize;
            let out = send_window(&mut e, &img, offset, seq);
            assert_eq!(out.effect, Some(Effect::ProgramBlock { index: block }));
            seq = seq.wrapping_add(86); // 342 frames per window, mod 256
            let ack = e.handle(Event::ProgramDone(true));
            assert_eq!(ack.reply, Some(ack_bytes(CMD_DATA, seq.wrapping_sub(1))));
        }
        assert_eq!(e.phase(), Phase::Transferred);
        // No ambiguity: 111 window-start seqs are pairwise distinct because
        // 86·k mod 256 has period 128 and 111 < 128.
        let mut sorted = start_seqs.clone();
        sorted.sort_unstable();
        sorted.dedup();
        assert_eq!(sorted.len(), 111, "window-start seqs must be unique");
    }

    // ---- VERIFY / metadata / GO ----

    #[test]
    fn verify_match_writes_metadata_then_acks() {
        let mut e = engine_in_phase(Phase::Transferred);
        let out = e.handle_frame(CMD_ID, &verify_frame(0xCAFE_F00D));
        assert_eq!(out.effect, Some(Effect::ComputeCrc { len: 16 }));
        assert_eq!(out.reply, None);
        let out = e.handle(Event::CrcResult(0xCAFE_F00D));
        assert_eq!(
            out.effect,
            Some(Effect::WriteMetadata {
                image_size: 16,
                image_crc: 0xCAFE_F00D
            })
        );
        assert_eq!(out.reply, None, "no ACK before the metadata commit");
        let out = e.handle(Event::MetadataDone(true));
        assert_eq!(out.reply, Some(ack_bytes(CMD_VERIFY, 0)));
        assert_eq!(e.phase(), Phase::Verified);
    }

    #[test]
    fn verify_mismatch_naks_err_crc_and_never_writes_metadata() {
        let mut e = engine_in_phase(Phase::Transferred);
        let out = e.handle_frame(CMD_ID, &verify_frame(0x1111_1111));
        assert_eq!(out.effect, Some(Effect::ComputeCrc { len: 16 }));
        let out = e.handle(Event::CrcResult(0x2222_2222));
        assert_eq!(out.reply, Some(nak_bytes(CMD_VERIFY, ERR_CRC, 0)));
        assert_eq!(out.effect, None, "metadata must stay erased on mismatch");
        assert_eq!(e.phase(), Phase::Transferred);
    }

    #[test]
    fn metadata_write_failure_naks_err_flash_and_stays_transferred() {
        let mut e = engine_in_phase(Phase::Transferred);
        e.handle_frame(CMD_ID, &verify_frame(0x3333_3333));
        e.handle(Event::CrcResult(0x3333_3333));
        let out = e.handle(Event::MetadataDone(false));
        assert_eq!(out.reply, Some(nak_bytes(CMD_VERIFY, ERR_FLASH, 0)));
        assert_eq!(e.phase(), Phase::Transferred);
    }

    #[test]
    fn go_acks_and_emits_reset() {
        let mut e = engine_in_phase(Phase::Verified);
        let out = e.handle_frame(CMD_ID, &go_frame());
        assert_eq!(out.reply, Some(ack_bytes(CMD_GO, 0)));
        assert_eq!(out.effect, Some(Effect::Reset));
    }

    // ---- power-loss ordering, asserted via the effect sequence ----

    #[test]
    fn effect_sequence_proves_power_loss_ordering() {
        let img = make_image(3000);
        let crc = 0xABCD_EF01u32;
        let mut e = engine();
        let mut effects = Vec::new();
        let mut record = |out: Output| {
            if let Some(fx) = out.effect {
                effects.push(fx);
            }
            out
        };
        record(hello(&mut e));
        record(e.handle_frame(CMD_ID, &size_frame(img.len() as u32)));
        record(e.handle_frame(CMD_ID, &erase_frame()));
        record(e.handle(Event::EraseDone(true)));
        let mut offset = 0usize;
        let mut seq = 0u8;
        while offset < img.len() {
            let target = (img.len() - offset).min(BLOCK_SIZE as usize);
            let frames = target.div_ceil(DATA_BYTES_PER_FRAME);
            record(send_window(&mut e, &img, offset, seq));
            record(e.handle(Event::ProgramDone(true)));
            seq = seq.wrapping_add(frames as u8);
            offset += target;
        }
        record(e.handle_frame(CMD_ID, &verify_frame(crc)));
        record(e.handle(Event::CrcResult(crc)));
        record(e.handle(Event::MetadataDone(true)));
        record(e.handle_frame(CMD_ID, &go_frame()));

        let pos = |fx: &Effect| effects.iter().position(|x| x == fx);
        let erase = pos(&Effect::EraseAll).expect("EraseAll emitted");
        let first_program = effects
            .iter()
            .position(|fx| matches!(fx, Effect::ProgramBlock { .. }))
            .expect("ProgramBlock emitted");
        let compute = pos(&Effect::ComputeCrc { len: 3000 }).expect("ComputeCrc emitted");
        let metadata = pos(&Effect::WriteMetadata {
            image_size: 3000,
            image_crc: crc,
        })
        .expect("WriteMetadata emitted");
        let reset = pos(&Effect::Reset).expect("Reset emitted");
        assert!(erase < first_program, "erase precedes all programming");
        let last_program = effects
            .iter()
            .rposition(|fx| matches!(fx, Effect::ProgramBlock { .. }))
            .expect("ProgramBlock emitted");
        assert!(
            last_program < compute && compute < metadata,
            "commit is last write"
        );
        assert_eq!(reset, effects.len() - 1, "reset is the final effect");
        assert_eq!(
            effects
                .iter()
                .filter(|fx| **fx
                    == Effect::WriteMetadata {
                        image_size: 3000,
                        image_crc: crc
                    })
                .count(),
            1
        );
    }

    // ---- INFO ----

    #[test]
    fn info_returns_send_info_and_never_touches_session_state() {
        let img = make_image(2 * BLOCK_SIZE as usize);
        let mut e = engine();
        enter_transfer(&mut e, img.len() as u32);
        assert_eq!(
            e.handle_frame(CMD_ID, &data_frame(0, &img[..6])),
            Output::none()
        );
        // INFO mid-window: served, state untouched.
        let out = e.handle_frame(CMD_ID, &[CMD_INFO, F, F, F, F, F, F, F]);
        assert_eq!(out.effect, Some(Effect::SendInfo));
        assert_eq!(out.reply, None);
        // The window continues seamlessly at seq 1.
        assert_eq!(
            e.handle_frame(CMD_ID, &data_frame(1, &img[6..12])),
            Output::none()
        );
    }

    #[test]
    fn info_reply_is_byte_exact() {
        let frames = build_info_reply(b"R4 CAN Boot");
        assert_eq!(frames[0], [0x79, 0x07, 11, 0x01, 0xFF, 0xFF, 0xFF, 0xFF]);
        assert_eq!(&frames[1], b"R4 CAN B");
        assert_eq!(&frames[2], b"oot\0\0\0\0\0");
        assert_eq!(frames[3], [0x00; 8]);
        assert_eq!(frames[4], [0x00; 8]);
    }

    #[test]
    fn info_reply_empty_name_is_all_zero_with_len_0() {
        let frames = build_info_reply(&[]);
        assert_eq!(frames[0], [0x79, 0x07, 0, 0x01, 0xFF, 0xFF, 0xFF, 0xFF]);
        for frame in &frames[1..] {
            assert_eq!(frame, &[0x00; 8]);
        }
    }

    #[test]
    fn info_reply_clamps_oversized_names_to_32_bytes() {
        let name = [b'A'; 40];
        let frames = build_info_reply(&name);
        assert_eq!(frames[0][2], 32);
        for frame in &frames[1..] {
            assert_eq!(frame, &[b'A'; 8]);
        }
    }

    // ---- robustness ----

    #[test]
    fn completion_events_with_nothing_pending_are_ignored() {
        for phase in [
            Phase::Idle,
            Phase::Greeted,
            Phase::Transfer,
            Phase::Verified,
        ] {
            let mut e = engine_in_phase(phase);
            for event in [
                Event::EraseDone(true),
                Event::EraseDone(false),
                Event::ProgramDone(true),
                Event::ProgramDone(false),
                Event::CrcResult(0),
                Event::MetadataDone(true),
                Event::MetadataDone(false),
            ] {
                assert_eq!(e.handle(event), Output::none());
                assert_eq!(e.phase(), phase);
            }
        }
    }

    #[test]
    fn mismatched_completion_kind_is_ignored_while_pending() {
        let mut e = engine();
        hello(&mut e);
        e.handle_frame(CMD_ID, &size_frame(16));
        assert_eq!(
            e.handle_frame(CMD_ID, &erase_frame()).effect,
            Some(Effect::EraseAll)
        );
        // Wrong completion kinds must not resolve the pending erase.
        assert_eq!(e.handle(Event::ProgramDone(true)), Output::none());
        assert_eq!(e.handle(Event::CrcResult(1)), Output::none());
        assert_eq!(e.handle(Event::MetadataDone(true)), Output::none());
        // The right one still does.
        assert_eq!(
            e.handle(Event::EraseDone(true)).reply,
            Some(ack_bytes(CMD_ERASE, 0))
        );
    }

    #[test]
    fn frames_during_pending_effect_nak_err_state_but_info_is_served() {
        let mut e = engine();
        hello(&mut e);
        e.handle_frame(CMD_ID, &size_frame(16));
        e.handle_frame(CMD_ID, &erase_frame());
        let out = e.handle_frame(CMD_ID, &data_frame(0, &[0; 6]));
        assert_eq!(out.reply, Some(nak_bytes(CMD_DATA, ERR_STATE, 0)));
        let out = e.handle_frame(CMD_ID, &[CMD_INFO, F, F, F, F, F, F, F]);
        assert_eq!(out.effect, Some(Effect::SendInfo));
        assert_eq!(
            e.handle(Event::EraseDone(true)).reply,
            Some(ack_bytes(CMD_ERASE, 0))
        );
    }

    /// Fixed table of adversarial frames (rules/testing.md fault injection):
    /// none may panic, and after the barrage a normal session must still run.
    #[test]
    fn adversarial_frames_never_panic_or_wedge_the_machine() {
        let adversarial: &[&[u8]] = &[
            &[],
            &[0x01],
            &[0x03, 0x00],
            &[0xFF; 8],
            &[0x00; 8],
            &[0x00; 9],
            &[CMD_HELLO, 0xFF, 0, 0, 0, 0, 0, 0],
            &[CMD_DATA, 0xFF, 1, 2, 3, 4, 5, 6],
            &[CMD_SIZE, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0],
            &[CMD_VERIFY, 0, 0xDE, 0xAD, 0xBE, 0xEF, 0, 0],
            &[CMD_GO, 0, 0, 0, 0, 0, 0, 0],
            &[0x08, 0x79, 0x79, 0x79, 0x79, 0x79, 0x79, 0x79],
            &[0x79, 0x01, 0x00, 0x01, 0x00, 0xB8, 0x03, 0x00], // our own reply echoed back
        ];
        let phases = [
            Phase::Idle,
            Phase::Greeted,
            Phase::Sized,
            Phase::Transfer,
            Phase::Transferred,
            Phase::Verified,
        ];
        for phase in phases {
            let mut e = engine_in_phase(phase);
            for data in adversarial {
                let _ = e.handle_frame(CMD_ID, data); // replies irrelevant: must not panic
                let _ = e.handle_frame(0x123, data);
            }
            // Not wedged: HELLO restarts and a full tiny session completes.
            assert_eq!(hello(&mut e).reply, Some(hello_ack_bytes()));
            let img = make_image(8);
            e.handle_frame(CMD_ID, &size_frame(8));
            e.handle_frame(CMD_ID, &erase_frame());
            e.handle(Event::EraseDone(true));
            transfer_image(&mut e, &img);
            e.handle_frame(CMD_ID, &verify_frame(7));
            e.handle(Event::CrcResult(7));
            e.handle(Event::MetadataDone(true));
            let out = e.handle_frame(CMD_ID, &go_frame());
            assert_eq!(out.effect, Some(Effect::Reset), "from phase {phase:?}");
        }
    }

    /// Adversarial frames while an effect is outstanding must not resolve or
    /// cancel it (except HELLO, tested elsewhere).
    #[test]
    fn adversarial_frames_during_pending_program_leave_it_pending() {
        let img = make_image(8);
        let mut e = engine();
        enter_transfer(&mut e, 8);
        assert_eq!(
            send_window(&mut e, &img, 0, 0).effect,
            Some(Effect::ProgramBlock { index: 0 })
        );
        for data in [&[0xFFu8; 8][..], &[0x03, 0x02, 0, 0, 0, 0, 0, 0], &[]] {
            let _ = e.handle_frame(CMD_ID, data);
        }
        assert_eq!(
            e.handle(Event::ProgramDone(true)).reply,
            Some(ack_bytes(CMD_DATA, 1)) // 2 frames: seqs 0, 1
        );
        assert_eq!(e.phase(), Phase::Transferred);
    }
}
