//
// Ra4m1Can0.cs — minimal FIFO-mailbox CAN0 controller model for the RA4M1
// bootloader's CAN update driver.  MIT-licensed; a test-time Renode peripheral,
// never linked into firmware.
//
// WHY THIS EXISTS
// ---------------
// Renode v1.16.1's stock `renesas-r7fa4m1a.repl` declares CAN0 only as a
// `Tag <0x40050000,0x40050877> "CAN0"` — an address reservation, not a model.
// So the bootloader's CAN0 driver (bootloader/src/drivers/can0.rs) cannot bring
// the controller up: STR never mirrors the requested CTLR.CANM mode, the RX FIFO
// never fills, and TX never completes.  This model implements just enough of the
// FIFO-mailbox register interface — bridged to Renode's stock CAN framework
// (ICAN + CANHub) — that the driver's init recipe, RX-FIFO drain, and mailbox-0
// TX all work, so the CAN update protocol runs end-to-end in emulation.
//
// WHAT THIS MODELS (only what can0.rs's FIFO-mailbox driver exercises)
// -------------------------------------------------------------------
//   - CTLR.CANM mode sequencing: STR's RSTST/HLTST/SLPST (bits 8/9/10) mirror
//     CANM so set_mode()'s `STR & 0x0700 == CANM<<8` polls resolve (can0.rs
//     STR_MODE_MASK / CanMode::str_bits).
//   - RX FIFO (4-deep): frames arriving from the CANHub are queued; the driver
//     drains them through the MB28 access window (mb28_id/mb28_dl/mb28_d0..7),
//     checks RFCR.RFEST (empty, bit 7), and advances with RFPCR = 0xFF.
//   - TX mailbox 0: MB0_ID/MB0_DL/MB0_D0..7 load a frame; MCTL_TX[0].TRMREQ
//     (bit 7) transmits it onto the hub and self-completes SENTDATA (bit 0);
//     the driver's double-write-0 clears it (can0.rs clear_tx_mctl).
//   - BCR / FIDCR / MKR / MKIVLR / MIER / EIER writes accepted (config recipe).
// Register offsets and bit positions are the RA4M1 `CAN0` block from the
// `ra4m1` 0.2.0 PAC (base 0x40050000; mailboxes @ 0x200 + n*0x10; MB_ID SID @
// bits 28:18; control block @ 0x840), which mirror R01UM0007's CAN chapter that
// can0.rs cites.
//
// WHAT IS FAKED / NOT MODELLED (honesty boundary — docs/plan-m2.5-renode.md §2)
// ----------------------------------------------------------------------------
//   - CAN IS FRAME-LEVEL, NOT BIT-LEVEL.  The 250 kbit/s bit timing, 75% sample
//     point, BRP prescaler, and SJW margin (can0.rs bit-timing constants) are
//     ACCEPTED into BCR but NOT exercised — no arbitration, no bus-off, no error
//     counters.  That physical layer stays the HIL bench `candump` job.
//   - The hardware exact-ID acceptance filter (FIDCR/MKR) is NOT enforced: every
//     classical standard data frame from the hub enters the FIFO.  The driver's
//     engine still filters by cmd-ID in software (protocol.rs on_frame), which
//     is what the tests rely on.  Extended and remote frames are dropped.
//   - No message-lost overwrite semantics beyond a coarse RFMLF flag; no TX
//     arbitration-loss / retry; STR.BOST is always 0 (bus-off never modelled).
//
using System;
using System.Collections.Generic;

using Antmicro.Renode.Core;
using Antmicro.Renode.Core.CAN;
using Antmicro.Renode.Logging;
using Antmicro.Renode.Peripherals;
using Antmicro.Renode.Peripherals.Bus;

namespace Antmicro.Renode.Peripherals.CAN
{
    // Multi-width by hand: the CAN0 block mixes u8 (mailbox data, MCTL, RFCR),
    // u16 (MB_DL, CTLR, STR, MKR) and u32 (MB_ID, BCR, FIDCR, MKIVLR) accesses.
    public class Ra4m1Can0 : IBytePeripheral, IWordPeripheral, IDoubleWordPeripheral, IKnownSize, ICAN
    {
        public Ra4m1Can0(IMachine machine)
        {
            rxFifo = new Queue<CANMessageFrame>();
            txData = new byte[8];
            Reset();
        }

        public event Action<CANMessageFrame> FrameSent;

        public void Reset()
        {
            ctlr = CtlrResetValue;
            rxFifo.Clear();
            rfe = false;
            rfmlf = false;
            sentdata = false;
            txId = 0;
            txDlc = 0;
            Array.Clear(txData, 0, txData.Length);
            fidcrSid = 0;
            SentCount = 0;
            LastSentId = 0;
        }

        // Test hooks (renode-test): observe frames the controller has transmitted
        // onto the hub, without a `Create CAN Tester` robot keyword (which does
        // not exist in Renode v1.16.1).  A monitor-readable stand-in for a bus
        // peer that counts replies and records the last reply's standard ID — the
        // symmetric counterpart to InjectStandardFrame.  Not silicon behaviour.
        public int SentCount { get; private set; }
        public uint LastSentId { get; private set; }

        public long Size => 0x878; // matches the stock CAN0 tag <0x40050000,0x40050877>

        // Frame-level RX from the CANHub.  Only classical standard data frames
        // reach this controller's FIFO (see class remarks on the faked filter).
        public void OnFrameReceived(CANMessageFrame message)
        {
            if(message.ExtendedFormat || message.RemoteFrame)
            {
                return;
            }
            if(!rfe)
            {
                return; // RX FIFO not enabled yet (before can0.rs init completes)
            }
            if(rxFifo.Count >= RxFifoDepth)
            {
                rfmlf = true; // FIFO full: message lost (overwrite mode)
                return;
            }
            rxFifo.Enqueue(message);
        }

        // Test hook (renode-test): inject a classical standard frame as if it
        // had been received on the bus, without needing a host SocketCAN bridge.
        // Not silicon behaviour — a monitor-callable stand-in for a bus peer.
        public void InjectStandardFrame(uint id, uint b0, uint b1, uint b2, uint b3,
                                        uint b4, uint b5, uint b6, uint b7)
        {
            var data = new byte[]
            {
                (byte)b0, (byte)b1, (byte)b2, (byte)b3,
                (byte)b4, (byte)b5, (byte)b6, (byte)b7,
            };
            OnFrameReceived(new CANMessageFrame(id & StandardIdMask, data));
        }

        // -------- byte-wide registers --------
        public byte ReadByte(long offset)
        {
            if(offset >= Mb28D0 && offset <= Mb28D7)
            {
                var frame = PeekRx();
                var i = (int)(offset - Mb28D0);
                return (frame != null && i < frame.Data.Length) ? frame.Data[i] : (byte)0;
            }
            switch(offset)
            {
            case MctlTx0:
                return sentdata ? (byte)0x01 : (byte)0x00; // SENTDATA = bit 0
            case Rfcr:
                return ReadRfcr();
            default:
                this.Log(LogLevel.Noisy, "Unhandled byte read @ 0x{0:X}", offset);
                return 0;
            }
        }

        public void WriteByte(long offset, byte value)
        {
            if(offset >= Mb0D0 && offset <= Mb0D7)
            {
                txData[offset - Mb0D0] = value;
                return;
            }
            switch(offset)
            {
            case MctlTx0:
                HandleTxRequest(value);
                break;
            case Rfcr:
                rfe = (value & 0x01) != 0; // RFE
                rfmlf = false;             // any RFCR write clears the lost flag
                break;
            case Rfpcr:
                if(value == 0xFF && rxFifo.Count > 0)
                {
                    rxFifo.Dequeue(); // advance CPU-side FIFO read pointer
                }
                break;
            default:
                // MCTL for the other mailboxes (disable writes), TFCR/TFPCR,
                // EIER etc. — accepted with no side effect.
                this.Log(LogLevel.Noisy, "Unhandled byte write @ 0x{0:X} = 0x{1:X}", offset, value);
                break;
            }
        }

        // -------- word-wide registers --------
        public ushort ReadWord(long offset)
        {
            switch(offset)
            {
            case Ctlr:
                return ctlr; // driver read-modify-writes CTLR (set_mode)
            case Str:
                return ReadStr();
            case Mb28Dl:
                var frame = PeekRx();
                return frame != null ? (ushort)(frame.Data.Length & 0xF) : (ushort)0;
            default:
                this.Log(LogLevel.Noisy, "Unhandled word read @ 0x{0:X}", offset);
                return 0;
            }
        }

        public void WriteWord(long offset, ushort value)
        {
            switch(offset)
            {
            case Ctlr:
                ctlr = value;
                break;
            case Mb0Dl:
                txDlc = (byte)(value & 0xF);
                break;
            default:
                // MKR[0..7] mask registers — accepted (faked acceptance filter).
                this.Log(LogLevel.Noisy, "Unhandled word write @ 0x{0:X} = 0x{1:X}", offset, value);
                break;
            }
        }

        // -------- double-word-wide registers --------
        public uint ReadDoubleWord(long offset)
        {
            switch(offset)
            {
            case Mb28Id:
                var frame = PeekRx();
                var sid = frame != null ? (frame.Id & StandardIdMask) : 0u;
                return sid << SidShift; // SID occupies MB_ID bits 28:18
            default:
                this.Log(LogLevel.Noisy, "Unhandled dword read @ 0x{0:X}", offset);
                return 0;
            }
        }

        public void WriteDoubleWord(long offset, uint value)
        {
            switch(offset)
            {
            case Mb0Id:
                txId = (value >> SidShift) & StandardIdMask;
                break;
            case Fidcr0:
                fidcrSid = (value >> SidShift) & StandardIdMask; // captured, not enforced
                break;
            default:
                // BCR bit timing, FIDCR1, MKIVLR, MIER — accepted, not exercised.
                this.Log(LogLevel.Noisy, "Unhandled dword write @ 0x{0:X} = 0x{1:X}", offset, value);
                break;
            }
        }

        private void HandleTxRequest(byte mctl)
        {
            if((mctl & 0x80) != 0) // TRMREQ
            {
                var data = new byte[txDlc];
                Array.Copy(txData, data, txDlc);
                FrameSent?.Invoke(new CANMessageFrame(txId, data));
                LastSentId = txId;
                SentCount++;
                sentdata = true; // instant transmit completion
            }
            else if(mctl == 0)
            {
                sentdata = false; // clear_tx_mctl double-write-0
            }
        }

        private byte ReadRfcr()
        {
            byte v = 0;
            if(rfe)
            {
                v |= 0x01; // RFE
            }
            var count = rxFifo.Count;
            v |= (byte)((count & 0x7) << 1); // RFUST (unread count, bits 1..3)
            if(rfmlf)
            {
                v |= 0x10; // RFMLF (bit 4)
            }
            if(count == 0)
            {
                v |= 0x80; // RFEST empty (bit 7)
            }
            return v;
        }

        private ushort ReadStr()
        {
            // STR mode flags mirror CTLR.CANM (bits 9:8) + SLPM (bit 10); the
            // driver polls STR & 0x0700 (RSTST/HLTST/SLPST).  BOST always 0.
            ushort str = 0;
            var canm = (ctlr >> 8) & 0x3;
            if(canm == 0x1)
            {
                str |= 1 << 8; // RSTST (reset mode)
            }
            else if(canm == 0x2)
            {
                str |= 1 << 9; // HLTST (halt mode)
            }
            if(((ctlr >> 10) & 0x1) != 0)
            {
                str |= 1 << 10; // SLPST (sleep)
            }
            return str;
        }

        private CANMessageFrame PeekRx()
        {
            return rxFifo.Count > 0 ? rxFifo.Peek() : null;
        }

        private readonly Queue<CANMessageFrame> rxFifo;
        private readonly byte[] txData;

        private ushort ctlr;
        private bool rfe;
        private bool rfmlf;
        private bool sentdata;
        private uint txId;
        private byte txDlc;
        private uint fidcrSid;

        private const int RxFifoDepth = 4;
        private const uint StandardIdMask = 0x7FF;
        private const int SidShift = 18;          // MB_ID SID field starts at bit 18
        private const ushort CtlrResetValue = 0x0500; // R01UM0007 CTLR reset value

        // CAN0 register offsets (ra4m1 0.2.0 PAC; base 0x40050000).
        private const long Mb0Id   = 0x200;
        private const long Mb0Dl   = 0x204;
        private const long Mb0D0   = 0x206;
        private const long Mb0D7   = 0x20D;
        private const long Mb28Id  = 0x3C0;
        private const long Mb28Dl  = 0x3C4;
        private const long Mb28D0  = 0x3C6;
        private const long Mb28D7  = 0x3CD;
        private const long Fidcr0  = 0x420;
        private const long MctlTx0 = 0x820; // mailbox 0 message-control (TX)
        private const long Ctlr    = 0x840;
        private const long Str     = 0x842;
        private const long Rfcr    = 0x848;
        private const long Rfpcr   = 0x849;
    }
}
