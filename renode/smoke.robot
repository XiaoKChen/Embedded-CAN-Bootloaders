*** Comments ***
# M2.5 minimal smoke suite for ra4m1-hal under Renode v1.16.1.
#
# Each case boots a REAL release example ELF (the artifact `cargo build
# --release --example` produces) on the UNO R4 Minima platform with the custom
# GPT320 compare-match model (renode/Ra4m1Gpt320.cs), then asserts the async
# executor is genuinely running off embassy-time. Both assertions FAIL on
# Renode's stock RenesasRA_GPT (proven: it has no GTCCRA compare-match, so the
# alarm never fires and the executor stalls after the first Timer) and PASS with
# the custom model.
#
# WHAT THIS MIRRORS FROM THE BENCH (docs/bench, docs/plan-m2.5-renode.md §2):
#   - Bench S2 "1 Hz blink" -> "Blinky Blinks The LED".
#   - Bench S2 "uptime heartbeat" / spike S-A "time still ticking" -> "Irq Smoke
#     Heartbeat Advances".
# WHAT IT DOES NOT COVER: wall-clock timing, HOCO tolerance, flash P/E timing,
# the P/E fetch-stall hazard, USB, CAN bit-timing. Those stay the HIL bench's
# job; a green run here never substitutes for the bench exit criteria.
#
# Run from the REPO ROOT (Renode cannot tokenize spaces/parens in @paths, so all
# paths are repo-relative and Renode's CWD must be the repo root):
#   <renode-install>/renode-test renode/smoke.robot

*** Variables ***
# Paths are absolute via ${CURDIR} (this .robot's directory) so they resolve
# regardless of Renode's own working directory under renode-test. ${CURDIR} is
# <repo>/renode; the ELFs live under <repo>/target.
${GPT_MODEL}          @${CURDIR}/Ra4m1Gpt320.cs
${PLATFORM}           @${CURDIR}/ra4m1-uno-r4.repl
${BLINKY_ELF}         @${CURDIR}/../target/thumbv7em-none-eabihf/release/examples/blinky_gpio
${IRQ_ELF}            @${CURDIR}/../target/thumbv7em-none-eabihf/release/examples/irq_smoke
# defmt-rtt _SEGGER_RTT up-channel 0 WrOff (bytes written). _SEGGER_RTT is at
# 0x20000008 in these builds; WrOff sits at control-block offset 0x24 -> 0x2000002C.
${RTT_WROFF}          0x2000002C

*** Keywords ***
Build Machine
    [Arguments]                     ${elf}
    Execute Command                 include ${GPT_MODEL}
    Execute Command                 mach create "uno-r4"
    Execute Command                 machine LoadPlatformDescription ${PLATFORM}
    Execute Command                 sysbus LoadELF ${elf}

Read RTT WrOff
    ${raw}=                         Execute Command    sysbus ReadDoubleWord ${RTT_WROFF}
    ${val}=                         Convert To Integer    ${raw.strip()}    16
    [Return]                        ${val}

*** Test Cases ***
Blinky Blinks The LED At 1 Hz
    [Documentation]                 Executor + GPT time driver alive: the D13/P111 LED
    ...                             toggles at 1 Hz virtual. Stuck HIGH on the stock model.
    Build Machine                   ${BLINKY_ELF}
    Create Led Tester               sysbus.port1.led_d13
    Start Emulation
    Assert Led Is Blinking          testDuration=2.1  onDuration=0.5  offDuration=0.5  tolerance=0.25  pauseEmulation=true

Irq Smoke Heartbeat Advances
    [Documentation]                 IEL2 (software-pended) wakes a task, then the
    ...                             embassy-time 1 s heartbeat keeps logging: the defmt-rtt
    ...                             WrOff strictly grows. Frozen after boot on the stock model.
    Build Machine                   ${IRQ_ELF}
    # Drive virtual time in bounded steps (RunFor pauses when done); do NOT
    # Start Emulation first — RunFor requires a paused machine.
    Execute Command                 emulation RunFor "0.5"
    ${w1}=                          Read RTT WrOff
    Execute Command                 emulation RunFor "4.0"
    ${w2}=                          Read RTT WrOff
    Should Be True                  ${w2} > ${w1}
    ...                             embassy-time heartbeat stalled: RTT WrOff did not advance (${w1} -> ${w2})
