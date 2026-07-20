//
// Ra4m1Spi.cs — a minimal RA4M1 RSPI (SPI0/SPI1) master model with internal
// loopback, for validating the ra4m1-hal SPI driver
// (crates/ra4m1-hal/src/spi.rs) under Renode v1.16.1.  MIT-licensed; a test-time
// Renode peripheral, never linked into firmware.
//
// WHY THIS EXISTS
// ---------------
// Renode v1.16.1's `renesas-r7fa4m1a.repl` models SPI0/SPI1 only as absorbing
// `Tag`s (see its sysbus init: `Tag <0x40072000,...> "SPI0"`).  There is no RA
// dedicated-SPI model, so the HAL's `SpiBus` cannot be exercised.  This models
// just the register handshake the driver's per-frame full-duplex loop drives.
//
// WHAT THIS MODELS (only what spi.rs exercises)
// ---------------------------------------------
// A single-buffered master where writing SPDR shifts a frame out AND — via an
// internal loopback (MOSI tied to MISO) — the same 8-bit frame back in, so SPRF
// asserts immediately after the SPDR write and reading SPDR returns that frame.
// Register offsets / bit positions are R01UM0007 (RA4M1 HW manual), Serial
// Peripheral Interface chapter, cross-checked against the register/bit names the
// driver uses through the `ra4m1` PAC (`spi0` block):
//   SPCR  @ 0x00 : SPTIE bit5, SPE bit6, SPRIE bit7, MSTR bit3   (spi.rs enable_*)
//   SPSR  @ 0x03 : SPTEF bit5 (tx-empty), SPRF bit7 (rx-full)    (spi.rs xfer_*)
//   SPDR  @ 0x04 : 32-bit data window (driver sets SPDCR.SPLW=1) (spi.rs xfer_*)
//   SPBR  @ 0x0A, SPDCR @ 0x0B, SPCMD0 @ 0x10 : config, stored not acted on
// Access widths mirror the PAC exactly: SPCR/SPSR/SPBR/SPDCR are byte, SPCMD0 is
// 16-bit, SPDR is 32-bit — so this implements IByte/IWord/IDoubleWordPeripheral
// and routes each width to the matching register.
//
// FIDELITY / WHAT IS FAKED
// ------------------------
//   - SPTEF reads as always-ready and, in the loopback, SPRF asserts the instant
//     SPDR is written; the driver's fast path (spi.rs xfer_async) therefore reads
//     SPTEF/SPRF synchronously and never parks, so the SPTI/SPRI interrupts are a
//     modelled-but-idle fallback here (UpdateInterrupts blinks them only if the
//     driver ever arms SPTIE/SPRIE, which this flow does not).
//   - No bit rate, no clock/mode timing, no SSL/CS, no OVRF/MODF/UDRF/PERF: this
//     is functional loopback only.  Do not read any timing or analog fact off it
//     (docs/plan-m2.5-renode.md §2; renode/TODO.md "register-logic, not chip-facts").
//   - The looped-back byte equals the written byte BY CONSTRUCTION, so equality
//     of TX and RX proves the driver completed the read phase (it read SPDR back),
//     not that a real device echoed anything.
//
using Antmicro.Renode.Core;
using Antmicro.Renode.Logging;
using Antmicro.Renode.Peripherals;
using Antmicro.Renode.Peripherals.Bus;

namespace Antmicro.Renode.Peripherals.SPI
{
    public class Ra4m1Spi : IDoubleWordPeripheral, IWordPeripheral, IBytePeripheral, IKnownSize
    {
        public Ra4m1Spi(IMachine machine)
        {
            ReceiveIRQ = new GPIO();
            TransmitIRQ = new GPIO();
            Reset();
        }

        public void Reset()
        {
            spcr = sslp = sppcr = spbr = spdcr = spckd = sslnd = spnd = spcr2 = 0;
            spcmd0 = 0;
            receiveFull = false;
            lastTx = lastRx = 0;
            firstCaptured = false;
            FirstByte = 0;
            TransferCount = 0;
            ReadCount = 0;
            ReceiveIRQ.Unset();
            TransmitIRQ.Unset();
        }

        public long Size => 0x20;

        // SPRI0 (receive-buffer-full) and SPTI0 (transmit-buffer-empty) events;
        // wired to the driver-assumed ICU events (spi.rs Spi0: SPRI 0xA1, SPTI
        // 0xA2) in the platform fragment.  BENCH-VERIFY those event numbers.
        public GPIO ReceiveIRQ { get; }
        public GPIO TransmitIRQ { get; }

        // Monitor-visible proof points for the Robot suite.
        public uint TransferCount { get; private set; } // SPDR writes (frames out)
        public uint ReadCount { get; private set; }     // SPDR reads (frames back)
        public uint FirstByte { get; private set; }     // first frame written (== 0xA5 seed)
        public uint LastByte => lastTx;                  // last frame written
        public uint LastRxByte => lastRx;                // last frame read back

        // --- 8-bit registers: SPCR, SSLP, SPPCR, SPSR, SPBR, SPDCR, SPCKD,
        //     SSLND, SPND, SPCR2 ---
        public byte ReadByte(long offset)
        {
            switch((Registers)offset)
            {
            case Registers.Control:                  return spcr;
            case Registers.SlaveSelectPolarity:      return sslp;
            case Registers.PinControl:               return sppcr;
            case Registers.Status:                   return StatusValue();
            case Registers.BitRate:                  return spbr;
            case Registers.DataControl:              return spdcr;
            case Registers.ClockDelay:               return spckd;
            case Registers.SlaveSelectNegationDelay: return sslnd;
            case Registers.NextAccessDelay:          return spnd;
            case Registers.Control2:                 return spcr2;
            default:
                this.Log(LogLevel.Noisy, "Unhandled byte read at offset 0x{0:X}", offset);
                return 0;
            }
        }

        public void WriteByte(long offset, byte value)
        {
            switch((Registers)offset)
            {
            case Registers.Control:                  spcr = value; UpdateInterrupts(); break;
            case Registers.SlaveSelectPolarity:      sslp = value; break;
            case Registers.PinControl:               sppcr = value; break;
            case Registers.Status:
                // W0C: the driver clears sticky errors by writing 0 to their bits
                // (spi.rs clear_errors).  We only maintain SPRF; a 0 in bit7 clears it.
                if((value & (1 << SprfBit)) == 0)
                {
                    receiveFull = false;
                }
                break;
            case Registers.BitRate:                  spbr = value; break;
            case Registers.DataControl:              spdcr = value; break;
            case Registers.ClockDelay:               spckd = value; break;
            case Registers.SlaveSelectNegationDelay: sslnd = value; break;
            case Registers.NextAccessDelay:          spnd = value; break;
            case Registers.Control2:                 spcr2 = value; break;
            default:
                this.Log(LogLevel.Noisy, "Unhandled byte write 0x{0:X} at offset 0x{1:X}", value, offset);
                break;
            }
        }

        // --- 16-bit register: SPCMD0 ---
        public ushort ReadWord(long offset)
        {
            if((Registers)offset == Registers.Command0)
            {
                return spcmd0;
            }
            this.Log(LogLevel.Noisy, "Unhandled word read at offset 0x{0:X}", offset);
            return 0;
        }

        public void WriteWord(long offset, ushort value)
        {
            if((Registers)offset == Registers.Command0)
            {
                spcmd0 = value;
                return;
            }
            this.Log(LogLevel.Noisy, "Unhandled word write 0x{0:X} at offset 0x{1:X}", value, offset);
        }

        // --- 32-bit register: SPDR (word-wide, SPDCR.SPLW = 1) ---
        public uint ReadDoubleWord(long offset)
        {
            if((Registers)offset == Registers.Data)
            {
                // Reading SPDR returns the looped-back frame and clears SPRF.
                receiveFull = false;
                ReadCount++;
                UpdateInterrupts();
                return lastRx;
            }
            this.Log(LogLevel.Noisy, "Unhandled dword read at offset 0x{0:X}", offset);
            return 0;
        }

        public void WriteDoubleWord(long offset, uint value)
        {
            if((Registers)offset == Registers.Data)
            {
                // Writing SPDR shifts a frame out and, in the internal loopback,
                // the same 8-bit frame back in: SPRF asserts immediately.
                lastTx = (byte)value;
                lastRx = (byte)value;
                receiveFull = true;
                TransferCount++;
                if(!firstCaptured)
                {
                    FirstByte = lastTx;
                    firstCaptured = true;
                }
                UpdateInterrupts();
                return;
            }
            this.Log(LogLevel.Noisy, "Unhandled dword write 0x{0:X} at offset 0x{1:X}", value, offset);
        }

        private byte StatusValue()
        {
            // SPTEF (tx-buffer-empty) always ready; SPRF reflects a pending
            // looped-back frame; IDLNF/OVRF/MODF/... are never asserted here.
            byte v = 1 << SptefBit;
            if(receiveFull)
            {
                v |= 1 << SprfBit;
            }
            return v;
        }

        private void UpdateInterrupts()
        {
            // Fallback path only — the driver's fast loop reads SPTEF/SPRF
            // synchronously and never arms these enables, so these normally stay
            // idle.  Modelled for fidelity to the "TX/RX IRQ" requirement.
            var sptie = (spcr & (1 << SptieBit)) != 0;
            var sprie = (spcr & (1 << SprieBit)) != 0;
            if(sptie)
            {
                TransmitIRQ.Blink();
            }
            if(sprie && receiveFull)
            {
                ReceiveIRQ.Blink();
            }
        }

        private byte spcr, sslp, sppcr, spbr, spdcr, spckd, sslnd, spnd, spcr2;
        private ushort spcmd0;
        private bool receiveFull;
        private bool firstCaptured;
        private byte lastTx, lastRx;

        private const int SptefBit = 5; // SPSR.SPTEF
        private const int SprfBit = 7;  // SPSR.SPRF
        private const int SptieBit = 5; // SPCR.SPTIE
        private const int SprieBit = 7; // SPCR.SPRIE

        private enum Registers
        {
            Control                  = 0x00, // SPCR
            SlaveSelectPolarity      = 0x01, // SSLP
            PinControl               = 0x02, // SPPCR
            Status                   = 0x03, // SPSR
            Data                     = 0x04, // SPDR (32-bit)
            BitRate                  = 0x0A, // SPBR
            DataControl              = 0x0B, // SPDCR
            ClockDelay               = 0x0C, // SPCKD
            SlaveSelectNegationDelay = 0x0D, // SSLND
            NextAccessDelay          = 0x0E, // SPND
            Control2                 = 0x0F, // SPCR2
            Command0                 = 0x10, // SPCMD0 (16-bit)
        }
    }
}
