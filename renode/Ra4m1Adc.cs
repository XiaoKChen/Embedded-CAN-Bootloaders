//
// Ra4m1Adc.cs — minimal ADC14 (S12AD) channel-scan model for the ra4m1-hal
// `adc.rs` driver.  MIT-licensed; a test-time Renode peripheral, never linked
// into firmware.
//
// WHY THIS EXISTS
// ---------------
// Renode v1.16.1's stock `renesas-r7fa4m1a.repl` maps ADC140 only as an absorbing
// sysbus Tag (<0x4005c000,0x4005c1e3>).  Against that Tag the driver's blocking
// `read()` reads ADCSR.ADST back as 0 (an unmapped read returns 0) and so thinks
// the scan finished instantly with an all-zero result, while the async
// `read_async()` never gets its S12ADI0 interrupt and hangs forever.  This model
// gives ADCSR/ADANSA/ADDR real state and raises the scan-complete event so BOTH
// the blocking and async paths in renode/TODO.md's ADC14 item can be checked.
//
// WHAT THIS MODELS (only what adc.rs exercises)
// ---------------------------------------------
//   - ADCSR (0x00, 16-bit): ADCS scan-mode (bits 13..14, stored) and ADST
//     (bit 15).  Writing ADST = 1 runs one scan *instantly* (Renode is
//     functional), so ADST reads back 0 — i.e. it SELF-CLEARS on scan complete,
//     which is exactly what `poll_until_done` waits on (adc.rs:281-288) and the
//     property adc.rs:31/269 documents.
//   - ADANSA0 (0x04) / ADANSA1 (0x06, 16-bit): channel select mask.  The scan
//     fills ADDR[c] for each selected channel c (adc.rs:264-267).
//   - ADCER (0x0E) / ADADC (0x0C): resolution/format + averaging, written once
//     at init (adc.rs:165-166).  Stored; they gate nothing in this model.
//   - ADDR[0..14] (0x20 + 2*n, 16-bit read-only): the 14-bit right-justified
//     result the driver reads with `addr[c].read()` (adc.rs:277).  Selected
//     channels return the injected sample; unselected channels read 0.
//   - S12ADI0 scan-complete event: on scan complete the model pulses GPIO
//     connection 0, wired in the platform to `icu@0x29`.  0x029 is the
//     `SCAN_END_EVENT` the driver routes to its IELSR slot
//     (adc.rs:74, enable_scan_interrupt / adc.rs:228).  The pulse (Set true then
//     false) matches the level-sensitive ICU handshake the GPT model documents:
//     the rising edge pends the NVIC line, the falling edge lets the bound
//     vector's IELSR.IR write-0-to-clear stick.
//
// Register offsets, the ADST bit position (bit 15), and the ADDR base (0x20,
// stride 2, 15 entries) are R01UM0007 (RA4M1 HW manual), 14-Bit A/D Converter
// (S12AD) chapter, cross-checked byte-for-byte against the `ra4m1` 0.2.0 PAC
// `adc140::RegisterBlock` (ADCSR@0x00 ADST<15> ADCS<13>, ADANSA0@0x04,
// ADANSA1@0x06, ADADC@0x0C, ADCER@0x0E, ADDR[15]@0x20).  Base address
// 0x4005_C000 is the PAC `ADC140::PTR`.
//
// CIRCULARITY CAVEAT (renode/TODO.md)
// -----------------------------------
// This validates the driver's register-LOGIC and its start -> scan-complete ->
// read flow (blocking self-clear + async IRQ), not silicon facts.  In particular
// the ICU event number 0x029 is asserted from adc.rs, so a Renode model coded
// from the same number would agree circularly — the event number itself is a
// BENCH-VERIFY item and must still be confirmed on silicon (adc.rs:71-74).  No
// analog front-end, conversion timing, resolution, or averaging is modelled.
//
// WHAT IS FAKED / NOT MODELLED
// ----------------------------
//   - No analog input.  Every selected channel returns the same injected
//     `sampleValue` (a fixed 14-bit count), so a test asserts the driver's
//     start/poll/read plumbing, NOT a real voltage.
//   - Conversion is instantaneous; no per-channel timing, ADHSC, or PCLKC
//     divider.  ADADC averaging and ADCER resolution are stored but ignored.
//   - Module-stop (MSTP.MSTPD16), group-B scans, and channels AN016+ (separate
//     ADDR16.. registers) are out of scope for the single-scan HAL path.
//
using System.Collections.Generic;
using System.Collections.ObjectModel;

using Antmicro.Renode.Core;
using Antmicro.Renode.Core.Structure.Registers;
using Antmicro.Renode.Logging;
using Antmicro.Renode.Peripherals.Bus;

namespace Antmicro.Renode.Peripherals.Analog
{
    // The driver accesses ADCSR/ADANSA/ADCER/ADDR as 16-bit and (via the enum
    // packing) some fall in the high half of a doubleword, so allow byte/word
    // accesses to be translated onto the natural doubleword registers.
    [AllowedTranslations(AllowedTranslation.ByteToDoubleWord | AllowedTranslation.WordToDoubleWord)]
    public class Ra4m1Adc : BasicDoubleWordPeripheral, INumberedGPIOOutput, IKnownSize
    {
        public Ra4m1Adc(IMachine machine, uint sampleValue = DefaultSampleValue) : base(machine)
        {
            this.sampleValue = (ushort)(sampleValue & ResultMask);
            results = new ushort[ChannelCount];

            scanEnd = new GPIO();
            Connections = new ReadOnlyDictionary<int, IGPIO>(
                new Dictionary<int, IGPIO> { { ScanEndConnection, scanEnd } });

            DefineRegisters();
        }

        public override void Reset()
        {
            base.Reset();
            for(var i = 0; i < results.Length; i++)
            {
                results[i] = 0;
            }
            scanEnd.Unset();
        }

        public long Size => 0x200;

        public IReadOnlyDictionary<int, IGPIO> Connections { get; }

        private void RunScan()
        {
            var mask = (uint)channelSelectA.Value;
            for(var ch = 0; ch < ChannelCount; ch++)
            {
                results[ch] = ((mask >> ch) & 1u) != 0 ? sampleValue : (ushort)0;
            }
            this.Log(LogLevel.Noisy, "S12AD scan complete: ADANSA0=0x{0:X}, sample=0x{1:X}", mask, sampleValue);

            // Scan-complete (S12ADI0) -> ICU event 0x029.  Level-sensitive
            // handshake, matching Ra4m1Gpt320's CCMPA pulse.
            scanEnd.Set(true);
            scanEnd.Set(false);
        }

        private void DefineRegisters()
        {
            Registers.Control.Define(this)                      // 0x00 ADCSR
                .WithValueField(0, 13, name: "ADCSR_LOW")       // DBLANS/GBADIE/DBLE/EXTRG/TRGE/ADHSC — stored
                .WithValueField(13, 2, name: "ADCS")            // scan-mode select — stored
                .WithFlag(15, name: "ADST",                     // start; self-clears on (instant) completion
                    valueProviderCallback: _ => false,
                    writeCallback: (_, v) => { if(v) RunScan(); })
                .WithReservedBits(16, 16);

            Registers.ChannelSelectA.Define(this)               // 0x04 ADANSA0 + 0x06 ADANSA1
                .WithValueField(0, 16, out channelSelectA, name: "ADANSA0")
                .WithValueField(16, 16, name: "ADANSA1");       // high-group select — stored, unused here

            Registers.ControlExtended.Define(this)              // 0x0C ADADC + 0x0E ADCER
                .WithValueField(0, 16, name: "ADADC")           // averaging — stored, ignored
                .WithValueField(16, 16, name: "ADCER");         // resolution/format — stored, ignored

            // ADDR[0..14] at 0x20 stride 2, read-only.  Two 16-bit results pack
            // into each doubleword: ADDR[2k] in the low half, ADDR[2k+1] in the
            // high half.  A 16-bit driver read (adc.rs:277) is translated onto
            // the enclosing doubleword by AllowedTranslation.WordToDoubleWord.
            for(var ch = 0; ch < ChannelCount; ch += 2)
            {
                var lo = ch;
                var hi = ch + 1;
                ((Registers)(DataBase + (ch / 2) * 4)).Define(this)
                    .WithValueField(0, 14, FieldMode.Read,
                        valueProviderCallback: _ => results[lo], name: "ADDR" + lo)
                    .WithReservedBits(14, 2)
                    .WithValueField(16, 14, FieldMode.Read,
                        valueProviderCallback: _ => hi < ChannelCount ? results[hi] : 0u, name: "ADDR" + hi)
                    .WithReservedBits(30, 2);
            }
        }

        private readonly ushort[] results;
        private readonly ushort sampleValue;
        private readonly GPIO scanEnd;
        private IValueRegisterField channelSelectA;

        private const int ScanEndConnection = 0;
        private const int ChannelCount = 15;            // AN000..AN014 -> ADDR[0..14]
        private const uint ResultMask = 0x3FFF;         // 14-bit right-justified
        private const uint DefaultSampleValue = 0x2AAA; // fixed injected count (10922); >>2 = 0x0AAA
        private const int DataBase = 0x20;              // ADDR[0]

        private enum Registers
        {
            Control         = 0x00,   // ADCSR (ADST<15>, ADCS<13:14>)
            ChannelSelectA  = 0x04,   // ADANSA0(0x04) + ADANSA1(0x06)
            ControlExtended = 0x0C,   // ADADC(0x0C) + ADCER(0x0E)
            // ADDR[0..14] at DataBase(0x20) + (ch/2)*4 are defined in a loop.
        }
    }
}
