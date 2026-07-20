//
// Ra4m1Gpt320.cs — minimal GPT320 channel-0 model for the ra4m1-hal
// embassy-time driver.  MIT-licensed; a test-time Renode peripheral, never
// linked into firmware.
//
// WHY THIS EXISTS
// ---------------
// Renode v1.16.1's stock `Timers.RenesasRA_GPT` cannot drive this firmware's
// time base.  Three gaps (all verified against the stock model source and
// against crates/ra4m1-embassy/src/time_driver.rs):
//
//   1. GTCR.CST (bit 0) is inert — the stock model only starts a channel on a
//      GTSTR (Software Start) write.  The HAL time driver starts the counter
//      with `gpt.gtcr.modify(|_, w| w.cst()._1())` (time_driver.rs:273), so on
//      the stock model the counter never runs.
//   2. TPCS (GTCR bits 23..26, the count-clock prescaler) is ignored.  Cosmetic
//      under Renode's virtual (non-timing-accurate) time — see the frequency
//      note below.
//   3. There is NO GTCCRA compare-match.  The stock model only blinks the
//      overflow/underflow IRQs; GTCCRA is a plain value field.  The HAL alarm
//      is the GTCCRA compare match (time_driver.rs:162-181, arm()):
//         GTCCRA == GTCNT  ->  GPT0 CCMPA event 0x057  ->  ICU IELSR0 -> NVIC
//         IEL0 (time_driver.rs:69-79, TIMER_NVIC_SLOT = 0).
//      Without it the alarm never fires and the executor idles forever after
//      the first Timer.
//
// WHAT THIS MODELS (only what the two smoke examples exercise)
// -----------------------------------------------------------
// GPT320 channel 0 as a FREE-RUNNING 32-bit up-counter (GTCNT) that wraps
// 0 -> 0xFFFFFFFF -> 0 without reset — the HAL's software 64-bit wrap-extension
// (time_driver.rs:18-30, now_locked) needs GTCNT to keep running past every
// compare — plus a GTCCRA compare that pulses the CCMPA output (connection 0)
// on match.  Register offsets are R01UM0007 (RA4M1 HW manual), General PWM
// Timer chapter, cross-checked byte-for-byte against the `ra4m1` 0.2.0 PAC
// `gpt320::RegisterBlock`.
//
// Connection 0 == CCMPA (compare match A), matching the stock model's IRQ
// bundling order (CCMPA, CCMPB, CMPC..F, OVF, UDF, PC) so the platform wiring
// `0 -> icu@0x57` reproduces the stock GPT0 CCMPA -> ICU event 0x057 link.
//
// FREQUENCY / TIME FIDELITY (honesty boundary)
// --------------------------------------------
// countFrequency defaults to 3 MHz to match the HAL's tick rate
// (DRIVER_TICK_HZ = PCLKD 48 MHz / 16, time_driver.rs:83-90), so virtual time
// tracks embassy-time 1:1 and blinky blinks at its intended 1 Hz *in virtual
// seconds*.  This is NOT wall-clock accurate: Renode is a functional emulator,
// TPCS is not applied, and no clock tolerance is modelled.  Do not read timing
// margins off this model (docs/plan-m2.5-renode.md §2).
//
// WHAT IS FAKED / NOT MODELLED
// ----------------------------
//   - GTPR is stored for read-back but does NOT resize the counter; the wrap
//     period is fixed at 2^32.  The HAL only ever writes GTPR = 0xFFFFFFFF
//     (time_driver.rs:252), which equals this fixed period, so the fake is
//     invisible to the firmware.
//   - Count direction is fixed ascending (the HAL always up-counts); GTUDDTYC
//     is accepted and stored but not acted on.
//   - GTST compare/overflow status flags are not maintained; the HAL clears the
//     interrupt at the ICU (IELSR.IR), not at GTST, so they are never read.
//   - Modes other than saw-wave, buffering, PWM output pins, and channels 1..7
//     are out of scope for the M2.5 minimal smoke.
//
using System.Collections.Generic;
using System.Collections.ObjectModel;

using Antmicro.Renode.Core;
using Antmicro.Renode.Core.Structure.Registers;
using Antmicro.Renode.Peripherals.Bus;
using Antmicro.Renode.Time;

namespace Antmicro.Renode.Peripherals.Timers
{
    [AllowedTranslations(AllowedTranslation.ByteToDoubleWord | AllowedTranslation.WordToDoubleWord)]
    public class Ra4m1Gpt320 : BasicDoubleWordPeripheral, INumberedGPIOOutput, IKnownSize
    {
        public Ra4m1Gpt320(IMachine machine, ulong countFrequency = DefaultCountFrequency) : base(machine)
        {
            ccmpa = new GPIO();
            Connections = new ReadOnlyDictionary<int, IGPIO>(
                new Dictionary<int, IGPIO> { { CcmpaConnection, ccmpa } });

            counter = new ComparingTimer(machine.ClockSource, countFrequency, this, "gpt0_cnt",
                limit: CounterMax, direction: Direction.Ascending, enabled: false,
                workMode: WorkMode.OneShot, eventEnabled: true, compare: CounterMax);
            counter.CompareReached += OnCompareReached;

            DefineRegisters();
        }

        public override void Reset()
        {
            base.Reset();
            counter.Reset();
            counter.EventEnabled = true;
            ccmpa.Unset();
        }

        public long Size => 0x100;

        public IReadOnlyDictionary<int, IGPIO> Connections { get; }

        private void OnCompareReached()
        {
            // GTCCRA compare match -> CCMPA event.  Pulse the line: the ICU
            // treats a peripheral event as level-sensitive (RenesasRA_ICU
            // IsEventTriggered returns `state` for events > external IRQs), so
            // the rising edge latches IELSR.IR/pends the NVIC and the falling
            // edge lets the driver's IR write-zero-to-clear (time_driver.rs:195)
            // stick instead of being immediately re-asserted.  The 32-bit
            // counter keeps running past the compare (free-running).
            ccmpa.Set(true);
            ccmpa.Set(false);
        }

        private void DefineRegisters()
        {
            Registers.WriteProtection.Define(this)              // 0x00 GTWP
                .WithValueField(0, 32);                         // key/WP accepted, ignored

            Registers.SoftwareStart.Define(this)                // 0x04 GTSTR
                .WithFlag(0, FieldMode.Write, name: "CSTRT0",
                    writeCallback: (_, v) => { if(v) counter.Enabled = true; })
                .WithReservedBits(1, 31);

            Registers.SoftwareStop.Define(this)                 // 0x08 GTSTP
                .WithFlag(0, FieldMode.Write, name: "CSTOP0",
                    writeCallback: (_, v) => { if(v) counter.Enabled = false; })
                .WithReservedBits(1, 31);

            Registers.SoftwareClear.Define(this)                // 0x0C GTCLR
                .WithFlag(0, FieldMode.Write, name: "CCLR0",
                    writeCallback: (_, v) => { if(v && !counter.Enabled) counter.Value = 0; })
                .WithReservedBits(1, 31);

            Registers.Control.Define(this)                      // 0x2C GTCR
                .WithFlag(0, name: "CST",                       // gap #1: honor CST
                    valueProviderCallback: _ => counter.Enabled,
                    writeCallback: (_, v) => counter.Enabled = v)
                .WithReservedBits(1, 15)
                .WithValueField(16, 3, name: "MD")             // saw-wave assumed; stored
                .WithReservedBits(19, 4)
                .WithValueField(23, 4, name: "TPCS")           // gap #2: prescaler cosmetic
                .WithReservedBits(27, 5);

            Registers.CountDirectionAndDuty.Define(this)        // 0x30 GTUDDTYC
                .WithValueField(0, 32);                         // ascending fixed; stored

            Registers.Status.Define(this)                       // 0x3C GTST
                .WithValueField(0, 32, FieldMode.Read);         // flags not maintained

            Registers.Counter.Define(this)                      // 0x48 GTCNT
                .WithValueField(0, 32, name: "GTCNT",
                    valueProviderCallback: _ => (uint)counter.Value,
                    writeCallback: (_, v) => { if(!counter.Enabled) counter.Value = v; });

            Registers.CompareCaptureA.Define(this)              // 0x4C GTCCRA
                .WithValueField(0, 32, name: "GTCCRA",          // gap #3: real compare
                    valueProviderCallback: _ => (uint)counter.Compare,
                    writeCallback: (_, v) => counter.Compare = v);

            Registers.CycleSetting.Define(this)                 // 0x64 GTPR
                .WithValueField(0, 32, name: "GTPR");           // stored; period fixed 2^32
        }

        private readonly ComparingTimer counter;
        private readonly GPIO ccmpa;

        private const int CcmpaConnection = 0;
        private const ulong CounterMax = 0xFFFFFFFF;
        private const ulong DefaultCountFrequency = 3000000;

        private enum Registers
        {
            WriteProtection          = 0x00,
            SoftwareStart            = 0x04,
            SoftwareStop             = 0x08,
            SoftwareClear            = 0x0C,
            Control                  = 0x2C,
            CountDirectionAndDuty    = 0x30,
            Status                   = 0x3C,
            Counter                  = 0x48,
            CompareCaptureA          = 0x4C,
            CycleSetting             = 0x64,
        }
    }
}
