//
// Ra4m1FlashLp.cs — minimal FACI-LP (R_FACI_LP) code/data-flash program/erase
// sequencer model for the RA4M1 bootloader's flash driver.  MIT-licensed; a
// test-time Renode peripheral, never linked into firmware.
//
// WHY THIS EXISTS
// ---------------
// Renode v1.16.1's stock `platforms/cpus/renesas-r7fa4m1a.repl` models code and
// data flash only as plain `Memory.MappedMemory` — there is NO flash program/
// erase controller (the 0x407EC000 region is a `Tag`).  So the bootloader's
// FACI-LP driver (bootloader/src/drivers/flash_lp.rs) can read flash but its
// erase/program sequences write into the void: FENTRYR never reads back the
// mode, FSTATR1.FRDY never sets, and every P/E call times out.  This model
// closes that gap so the driver's real erase -> program -> read-back loop runs.
//
// WHAT THIS MODELS (only what flash_lp.rs's driver code paths exercise)
// --------------------------------------------------------------------
// The FACI-LP command interface as a *functional* sequencer with INSTANT
// completion:
//   - FENTRYR P/E-mode entry/exit (CF 0xAA01 -> reads 0x0001; DF 0xAA80 ->
//     reads 0x0080; read 0xAA00 -> reads 0x0000), matching the driver's
//     mode-confirmation polls (flash_lp.rs ram_cf_pe_enter / ram_pe_exit,
//     FENTRYR_CF_PE_ACTIVE = 0x0001).
//   - FSTATR1.FRDY (bit 6): set on an FCR command, cleared on FCR = 0x00 —
//     the exact handshake ram_command_finish polls.
//   - FCR ERASE (0x84): fills [FSAR..FEAR] of the backing code flash with 0xFF.
//   - FCR PROGRAM (0x81): writes the 8-byte FWBL0/FWBH0/FWBL1/FWBH1 unit to
//     FSAR in the backing code flash (little-endian, per faci::write_program_data).
//   - FSTATR2 (error flags) reads 0 — no injected P/E errors.
//   - DFLCTL / FPMCR / FPR / FASR / FISR / FRESETR accepted so the driver's
//     open/gate/reset sequences (open_faci, data_flash_enable, ram_df_reset)
//     complete.
// Programmed/erased bytes land in the sysbus-mapped code flash, so subsequent
// CPU reads (flash_lp.rs verify()/read()) reflect them — the read-back the
// S3 bench loop asserts.
//
// Register offsets are the `R_FACI_LP_Type` struct offsets from the vendored
// Renesas CMSIS header (R7FA4M1AB.h, R_FACI_LP_BASE = 0x407EC000), byte-for-byte
// the offsets flash_lp.rs's `faci` module already documents; sequence semantics
// are R01UM0007's flash-memory chapter.
//
// WHAT IS FAKED / NOT MODELLED (honesty boundary — docs/plan-m2.5-renode.md §2)
// ----------------------------------------------------------------------------
//   - REAL P/E TIMING AND THE FETCH-STALL HAZARD ARE NOT MODELLED.  On silicon
//     the CPU CANNOT fetch from code flash while the sequencer is in code-flash
//     P/E mode — the entire reason flash_lp.rs forces its P/E leaves RAM-resident
//     (#[link_section=".data"]).  This model completes every command instantly
//     with no stall, so a RAM-residency regression that would BRICK real
//     hardware passes silently here.  That hazard stays the HIL bench's job.
//   - No program-once/erased-state enforcement, no ILGLERR/PRGERR/ERERR error
//     injection, no access-window/OCD/extra-area behaviour, no data-flash P/E
//     (the driver's v1 doesn't program data flash; only its FISR/reset round
//     trips run here).  FPMCR mode encodings are accepted but not validated.
//
using System;

using Antmicro.Renode.Core;
using Antmicro.Renode.Logging;
using Antmicro.Renode.Peripherals;
using Antmicro.Renode.Peripherals.Bus;

namespace Antmicro.Renode.Peripherals.MTD
{
    // Multi-width by hand: the FACI register block mixes u8/u16/u32 accesses and
    // FENTRYR sits at the unaligned offset 0x3FB2, which the register-collection
    // framework cannot express — so dispatch each access width by offset.
    public class Ra4m1FlashLp : IBytePeripheral, IWordPeripheral, IDoubleWordPeripheral, IKnownSize
    {
        public Ra4m1FlashLp(IMachine machine)
        {
            this.machine = machine;
            Reset();
        }

        public void Reset()
        {
            fentryMode = 0;
            frdy = true;      // sequencer ready when idle (FSP assumes FRDY=1)
            dflctl = 0;
            fpmcr = FpmcrReadMode;
            fasr = 0;
            fcr = 0;
            fsarL = fsarH = 0;
            fearL = fearH = 0;
            fisr = 0;
            fwbl0 = fwbh0 = fwbl1 = fwbh1 = 0;
        }

        public long Size => 0x4000; // covers up to FENTRYR @ 0x3FB2

        // -------- byte-wide registers --------
        public byte ReadByte(long offset)
        {
            switch((Reg)offset)
            {
            case Reg.DFLCTL: return dflctl;
            case Reg.FPMCR:  return fpmcr;
            case Reg.FASR:   return fasr;
            case Reg.FCR:    return fcr;
            default:
                this.Log(LogLevel.Noisy, "Unhandled byte read @ 0x{0:X}", offset);
                return 0;
            }
        }

        public void WriteByte(long offset, byte value)
        {
            switch((Reg)offset)
            {
            case Reg.DFLCTL: dflctl = value; break; // DFLEN data-flash read enable
            case Reg.FPMCR:  fpmcr = value; break;  // P/E mode control (accepted)
            case Reg.FASR:   fasr = value; break;   // area select (user area = 0)
            case Reg.FCR:    HandleFcr(value); break;
            default:
                this.Log(LogLevel.Noisy, "Unhandled byte write @ 0x{0:X} = 0x{1:X}", offset, value);
                break;
            }
        }

        // -------- word-wide registers --------
        public ushort ReadWord(long offset)
        {
            switch((Reg)offset)
            {
            case Reg.FENTRYR: return fentryMode; // 0x0001 CF / 0x0080 DF / 0 read
            default:
                this.Log(LogLevel.Noisy, "Unhandled word read @ 0x{0:X}", offset);
                return 0;
            }
        }

        public void WriteWord(long offset, ushort value)
        {
            switch((Reg)offset)
            {
            case Reg.FSARL: fsarL = value; break;
            case Reg.FSARH: fsarH = value; break;
            case Reg.FEARL: fearL = value; break;
            // FENTRYR key is the high byte (0xAA); the low byte selects the mode.
            case Reg.FENTRYR: fentryMode = (ushort)(value & 0xFF); break;
            default:
                this.Log(LogLevel.Noisy, "Unhandled word write @ 0x{0:X} = 0x{1:X}", offset, value);
                break;
            }
        }

        // -------- double-word-wide registers --------
        public uint ReadDoubleWord(long offset)
        {
            switch((Reg)offset)
            {
            case Reg.FSTATR1: return frdy ? FrdyBit : 0u; // FRDY = bit 6
            case Reg.FSTATR2: return 0;                    // no P/E errors injected
            case Reg.FISR:    return fisr;
            default:
                this.Log(LogLevel.Noisy, "Unhandled dword read @ 0x{0:X}", offset);
                return 0;
            }
        }

        public void WriteDoubleWord(long offset, uint value)
        {
            switch((Reg)offset)
            {
            case Reg.FEARH:   fearH = (ushort)(value & 0xFFFF); break;
            case Reg.FRESETR: break;              // FRESET pulse (accepted)
            case Reg.FWBL0:   fwbl0 = value; break;
            case Reg.FWBH0:   fwbh0 = value; break;
            case Reg.FWBL1:   fwbl1 = value; break;
            case Reg.FWBH1:   fwbh1 = value; break;
            case Reg.FPR:     break;              // protection unlock (accepted)
            case Reg.FISR:    fisr = value; break;
            default:
                this.Log(LogLevel.Noisy, "Unhandled dword write @ 0x{0:X} = 0x{1:X}", offset, value);
                break;
            }
        }

        private void HandleFcr(byte value)
        {
            fcr = value;
            if(value == FcrClear)
            {
                frdy = false; // ram_command_finish: after clear, FRDY drops to 0
                return;
            }

            var startAddr = ((uint)fsarH << 16) | fsarL;
            var bus = machine.GetSystemBus(this);
            if(value == FcrErase)
            {
                var endAddr = ((uint)fearH << 16) | fearL; // inclusive last byte
                for(var addr = startAddr; addr <= endAddr; addr++)
                {
                    bus.WriteByte(addr, 0xFF);
                }
            }
            else if(value == FcrProgram)
            {
                // 64-bit write unit, little-endian across FWBL0/FWBH0/FWBL1/FWBH1.
                bus.WriteByte(startAddr + 0, (byte)(fwbl0 & 0xFF));
                bus.WriteByte(startAddr + 1, (byte)((fwbl0 >> 8) & 0xFF));
                bus.WriteByte(startAddr + 2, (byte)(fwbh0 & 0xFF));
                bus.WriteByte(startAddr + 3, (byte)((fwbh0 >> 8) & 0xFF));
                bus.WriteByte(startAddr + 4, (byte)(fwbl1 & 0xFF));
                bus.WriteByte(startAddr + 5, (byte)((fwbl1 >> 8) & 0xFF));
                bus.WriteByte(startAddr + 6, (byte)(fwbh1 & 0xFF));
                bus.WriteByte(startAddr + 7, (byte)((fwbh1 >> 8) & 0xFF));
            }
            else
            {
                this.Log(LogLevel.Warning, "Unmodelled FCR command 0x{0:X}", value);
            }
            frdy = true; // instant completion — command done, FRDY set
        }

        private readonly IMachine machine;

        private ushort fentryMode;
        private bool frdy;
        private byte dflctl;
        private byte fpmcr;
        private byte fasr;
        private byte fcr;
        private ushort fsarL, fsarH;
        private ushort fearL, fearH;
        private uint fisr;
        private uint fwbl0, fwbh0, fwbl1, fwbh1;

        private const uint FrdyBit = 1u << 6; // FSTATR1.FRDY
        private const byte FcrProgram = 0x81;
        private const byte FcrErase = 0x84;
        private const byte FcrClear = 0x00;
        private const byte FpmcrReadMode = 0x08;

        // R_FACI_LP_Type struct offsets (R7FA4M1AB.h; == flash_lp.rs faci module).
        private enum Reg : long
        {
            DFLCTL  = 0x090,
            FPMCR   = 0x100,
            FASR    = 0x104,
            FSARL   = 0x108,
            FSARH   = 0x110,
            FCR     = 0x114,
            FEARL   = 0x118,
            FEARH   = 0x120,
            FRESETR = 0x124,
            FSTATR1 = 0x12C,
            FWBL0   = 0x130,
            FWBH0   = 0x138,
            FWBL1   = 0x140,
            FWBH1   = 0x144,
            FPR     = 0x180,
            FISR    = 0x1D8,
            FSTATR2 = 0x1F0,
            FENTRYR = 0x3FB2,
        }
    }
}
