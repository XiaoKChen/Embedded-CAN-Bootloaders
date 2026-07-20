*** Comments ***
# M2.5 analog-peripheral suite for ra4m1-hal under Renode v1.16.1: ADC14 (S12AD)
# + DAC12, the two "Tag today" items in renode/TODO.md.
#
# Renode's stock renesas-r7fa4m1a.repl maps ADC140 (<0x4005c000,0x4005c1e3>) and
# DAC12 (<0x4005e000,0x4005e00b>) as absorbing sysbus Tags only, so the M3
# `adc.rs`/`dac.rs` register writes have no observable effect. This suite attaches
# the custom Ra4m1Adc.cs / Ra4m1Dac.cs models (compiled at runtime via `include`,
# no Renode rebuild) on top of the base platform and asserts the drivers'
# register-LOGIC.
#
# WHAT THIS MIRRORS (docs/bench, renode/TODO.md "HAL peripheral coverage"):
#   - DAC12 item: "set a 12-bit value, assert DADR/output register reads back".
#   - ADC14 item: "ADCSR scan (ADST self-clear), ADDR[n] results, S12ADI IRQ;
#     blocking read() + async read_async()".
#
# TWO PATHS, TWO CASES:
#   1. "Blocking Scan ..." drives the ADC registers directly in the exact order
#      Adc::read() does (adc.rs:182-190) — no example ELF calls the blocking
#      read(), so this exercises the register interface a blocking driver drives:
#      ADST self-clears on (instant) scan complete and ADDR[c] latches the result.
#   2. "Async Read Mirrors ..." boots the REAL `analog` example ELF, whose
#      read_async() (bound to IEL2) awaits the S12ADI0 scan-complete IRQ and
#      mirrors the 14-bit A0 sample to the 12-bit DAC (raw >> 2). This proves the
#      ADC IRQ path (event 0x29 -> ICU -> IEL2 -> waker) and the DAC write chain.
#
# ELF DEPENDENCY: case 2 requires the release `analog` example ELF. Build it from
# the repo root before running:
#   cargo build --release -p ra4m1-hal --example analog --target thumbv7em-none-eabihf
# Case 1 needs no ELF (pure model register logic).
#
# CIRCULARITY / HONESTY (renode/TODO.md, docs/plan-m2.5-renode.md §2): this
# validates register-logic, not silicon facts. The injected ADC sample is a fixed
# digital count (no analog front-end), the DAC output is a value register (no real
# voltage), and the S12ADI ICU event number 0x29 is taken from adc.rs, so a model
# coded from the same number agrees circularly — the event number stays a
# BENCH-VERIFY item. No conversion timing, resolution, reference, or settling is
# modelled; those remain the HIL bench's job.
#
# Run from the REPO ROOT:
#   <renode-install>/renode-test renode/analog.robot

*** Variables ***
# ${CURDIR} is <repo>/renode; models sit beside this file, ELFs under <repo>/target.
${GPT_MODEL}      @${CURDIR}/Ra4m1Gpt320.cs
${DAC_MODEL}      @${CURDIR}/Ra4m1Dac.cs
${ADC_MODEL}      @${CURDIR}/Ra4m1Adc.cs
${PLATFORM}       @${CURDIR}/ra4m1-uno-r4.repl
${ANALOG_ELF}     @${CURDIR}/../target/thumbv7em-none-eabihf/release/examples/analog

# Register addresses (RA4M1 memory map; PAC ADC140::PTR / DAC12::PTR).
${ADCSR}          0x4005C000
${ADANSA0}        0x4005C004
${ADANSA1}        0x4005C006
${ADDR0}          0x4005C020
${ADDR1}          0x4005C022
${DADR0}          0x4005E000
${DACR}           0x4005E004

# The model's injected 14-bit sample and its 12-bit DAC mirror (analog.rs: raw>>2).
${SAMPLE}         ${10922}
${SAMPLE_DAC}     ${2730}

*** Keywords ***
Build Analog Machine
    [Arguments]                     ${elf}=${EMPTY}
    Execute Command                 include ${GPT_MODEL}
    Execute Command                 include ${DAC_MODEL}
    Execute Command                 include ${ADC_MODEL}
    Execute Command                 mach create "uno-r4-analog"
    Execute Command                 machine LoadPlatformDescription ${PLATFORM}
    Execute Command                 machine LoadPlatformDescriptionFromString "adc: Analog.Ra4m1Adc @ sysbus 0x4005c000 { 0 -> icu@0x29 }"
    Execute Command                 machine LoadPlatformDescriptionFromString "dac: Analog.Ra4m1Dac @ sysbus 0x4005e000"
    Run Keyword If                  "${elf}" != "${EMPTY}"    Execute Command    sysbus LoadELF ${elf}

Read Word
    [Arguments]                     ${addr}
    ${raw}=                         Execute Command    sysbus ReadWord ${addr}
    ${val}=                         Convert To Integer    ${raw.strip()}    16
    [Return]                        ${val}

Read DoubleWord
    [Arguments]                     ${addr}
    ${raw}=                         Execute Command    sysbus ReadDoubleWord ${addr}
    ${val}=                         Convert To Integer    ${raw.strip()}    16
    [Return]                        ${val}

*** Test Cases ***
Blocking Scan Self Clears Adst And Fills Addr
    [Documentation]                 Mirrors Adc::read() (adc.rs:182-190): select
    ...                             channel -> ADCSR.ADST=1 -> ADST self-clears on
    ...                             scan complete -> read ADDR[c]. No ELF: drives the
    ...                             register interface a blocking driver would drive.
    Build Analog Machine
    # select channel 0 (ADANSA0 = 1<<0), clear the high group (adc.rs:265-266)
    Execute Command                 sysbus WriteWord ${ADANSA0} 0x0001
    Execute Command                 sysbus WriteWord ${ADANSA1} 0x0000
    # start scan: ADCSR.ADST = bit 15 (adc.rs:271)
    Execute Command                 sysbus WriteWord ${ADCSR} 0x8000
    # ADST self-cleared on the (instant) scan complete (adc.rs:283 poll target)
    ${adcsr}=                       Read Word    ${ADCSR}
    ${adst}=                        Evaluate    ${adcsr} & 0x8000
    Should Be Equal As Integers     ${adst}    0    ADCSR.ADST did not self-clear after scan
    # selected channel returns the injected sample; unselected reads 0 (adc.rs:277)
    ${r0}=                          Read Word    ${ADDR0}
    Should Be Equal As Integers     ${r0}    ${SAMPLE}    ADDR[0] did not latch the scan sample
    ${r1}=                          Read Word    ${ADDR1}
    Should Be Equal As Integers     ${r1}    0    ADDR[1] (unselected channel) should read 0

Async Read Mirrors Adc To Dac
    [Documentation]                 Boots the real `analog` ELF: read_async() (IEL2)
    ...                             awaits the S12ADI0 IRQ, then mirrors the 14-bit A0
    ...                             sample to the 12-bit DAC (raw>>2). Proves the ADC
    ...                             IRQ path (0x29 -> ICU -> IEL2 -> waker) + DAC write.
    ...                             On the stock Tag the async read hangs (no IRQ).
    Build Analog Machine            ${ANALOG_ELF}
    # Bounded, deterministic virtual-time step; the first mirror iteration runs
    # well within this. RunFor requires a paused machine (do not Start first).
    Execute Command                 emulation RunFor "0.5"
    # DAC output enabled by Dac::new() (dac.rs:99): DACR.DAOE0 (bit 6) set.
    ${dacr}=                        Read DoubleWord    ${DACR}
    ${daoe0}=                       Evaluate    ${dacr} & 0x40
    Should Be True                  ${daoe0} != 0    DACR.DAOE0 not set: DAC output never enabled
    # DAC data = injected ADC sample >> 2 -> the async mirror chain wrote it.
    ${dadr}=                        Read DoubleWord    ${DADR0}
    Should Be Equal As Integers     ${dadr}    ${SAMPLE_DAC}
    ...                             DAC DADR != (ADC sample >> 2): async ADC->DAC mirror did not run
    # ADC result register still holds the raw 14-bit A0 sample.
    ${raw}=                         Read Word    ${ADDR0}
    Should Be Equal As Integers     ${raw}    ${SAMPLE}    ADDR[0] lost the raw A0 sample
