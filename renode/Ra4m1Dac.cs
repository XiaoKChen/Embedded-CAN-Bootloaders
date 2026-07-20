//
// Ra4m1Dac.cs — minimal DAC12 channel-0 model for the ra4m1-hal `dac.rs`
// driver.  MIT-licensed; a test-time Renode peripheral, never linked into
// firmware.
//
// WHY THIS EXISTS
// ---------------
// Renode v1.16.1's stock `renesas-r7fa4m1a.repl` maps DAC12 only as an absorbing
// sysbus Tag (<0x4005e000,0x4005e00b>) — reads return 0, writes are swallowed.
// The M3 `dac.rs` driver's register writes therefore have no observable effect,
// so the DAC12 acceptance item in renode/TODO.md ("set a 12-bit value, assert
// DADR/output register reads back") cannot be checked.  This model gives DADR0 /
// DACR real read/write state so a test can assert the driver's writes landed.
//
// WHAT THIS MODELS (only what dac.rs exercises)
// ---------------------------------------------
//   - DADR0 (0x00, 16-bit): the 12-bit right-justified conversion value the
//     driver writes with `dadr0.write` (dac.rs:98,110).  Low 12 bits stored and
//     read back; high 4 reserved (right-justified format, DADPR.DPSEL = 0).
//   - DACR (0x04, 8-bit): output-enable control.  DAOE0 (bit 6) is the driver's
//     `dacr.modify(w.daoe0())` (dac.rs:99,116,122) — stored and read back so a
//     test can confirm the channel was enabled/disabled.
//   - DADPR (0x05) / DAADSCR (0x06) / DAVREFCR (0x07): format / A-D-sync /
//     reference-select bytes the driver writes once at init (dac.rs:92-96).
//     Stored for read-back; they gate nothing in this functional model.
//
// Register offsets and the DAOE0 bit position (bit 6) are R01UM0007 (RA4M1 HW
// manual), 12-Bit D/A Converter chapter, cross-checked byte-for-byte against the
// `ra4m1` 0.2.0 PAC `dac12::RegisterBlock` (DADR0@0x00, DACR@0x04 DAOE0<6>,
// DADPR@0x05, DAADSCR@0x06, DAVREFCR@0x07) and against dac.rs's own register
// recipe.  Base address 0x4005_E000 is the PAC `DAC12::PTR`.
//
// CIRCULARITY CAVEAT (renode/TODO.md)
// -----------------------------------
// This validates the driver's register-LOGIC (the right offsets/bits are touched
// and read back), not silicon facts.  A model coded from the same assumptions as
// the driver would agree circularly; the real analog output voltage, the tDACR
// settling time, and the AVCC0/AVSS0 reference are NOT modelled — those stay the
// HIL bench's job (docs/plan-m2.5-renode.md §2).
//
// WHAT IS FAKED / NOT MODELLED
// ----------------------------
//   - No analog output.  DADR0 is a value register, not a voltage; nothing
//     converts it.  A test reads the digital DADR back, not a pin level.
//   - tDACR settling wait (dac.rs "Settling time") is not modelled — the value is
//     valid instantly.
//   - Module-stop (MSTP.MSTPD20) gating is not enforced; registers respond
//     whether or not the module was released.  MSTP is a stock absorbing Tag.
//
using Antmicro.Renode.Core;
using Antmicro.Renode.Core.Structure.Registers;
using Antmicro.Renode.Peripherals.Bus;

namespace Antmicro.Renode.Peripherals.Analog
{
    // DADR0 is written 16-bit; DACR/DADPR/DAADSCR/DAVREFCR are written 8-bit
    // (dac.rs uses `.write`/`.modify` on the byte-wide DACR at 0x04).  The four
    // control bytes 0x04..0x07 pack into the doubleword at 0x04, so byte writes
    // land as read-modify-writes of that word via ByteToDoubleWord.
    [AllowedTranslations(AllowedTranslation.ByteToDoubleWord | AllowedTranslation.WordToDoubleWord)]
    public class Ra4m1Dac : BasicDoubleWordPeripheral, IKnownSize
    {
        public Ra4m1Dac(IMachine machine) : base(machine)
        {
            DefineRegisters();
        }

        public long Size => 0x100;

        private void DefineRegisters()
        {
            Registers.Data.Define(this)                         // 0x00 DADR0
                .WithValueField(0, 12, name: "DADR")            // 12-bit value, stored/read-back
                .WithReservedBits(12, 20);                      // high 4 fixed 0 (right-justified) + word-pad

            Registers.Control.Define(this)                      // 0x04 DACR + DADPR + DAADSCR + DAVREFCR
                .WithReservedBits(0, 6)                         // DACR bits 0..5 unused on ch0-only part
                .WithFlag(6, name: "DAOE0")                     // output enable, stored/read-back
                .WithReservedBits(7, 1)                         // DACR bit 7 (DAE) unused here
                .WithValueField(8, 8, name: "DADPR")            // 0x05 format-select, stored
                .WithValueField(16, 8, name: "DAADSCR")         // 0x06 A/D-sync, stored
                .WithValueField(24, 8, name: "DAVREFCR");       // 0x07 reference-select, stored
        }

        private enum Registers
        {
            Data    = 0x00,   // DADR0 (16-bit; low 12 used)
            Control = 0x04,   // DACR(0x04) DADPR(0x05) DAADSCR(0x06) DAVREFCR(0x07)
        }
    }
}
