*** Comments ***
# M2.5 bootloader-integration suite for the RA4M1 under Renode v1.16.1.
#
# Boots the REAL bootloader release ELF (the artifact `cargo build --release -p
# bootloader` produces at target/thumbv7em-none-eabihf/release/bootloader) on the
# UNO R4 Minima platform, augmented with three custom models, and exercises the
# M0-M2 integration paths that boot-logic host tests structurally cannot reach:
#
#   - renode/Ra4m1Gpt320.cs   GPT320 time base (executor ticks; existing model).
#   - renode/Ra4m1FlashLp.cs  FACI-LP flash P/E sequencer -> backing code flash.
#   - renode/Ra4m1Can0.cs     CAN0 FIFO-mailbox controller <-> Renode CANHub.
#   - a MappedMemory at 0x4001E500 for the VBTBKR backup-register handoff words.
#
# WHAT EACH CASE MIRRORS FROM THE BENCH (docs/bench, docs/plan-m2.5-renode.md §2):
#   - S0 banner                         -> "Bootloader Boots To First Log Line".
#   - S5 VBTBKR round-trip / boot decn  -> "VBTBKR Magic Survives CPU Reset...".
#   - M1 CAN HELLO/--info reply          -> "CAN Update HELLO Gets A Reply".
#   - S3 erase -> program -> read-back    -> "FACI Erase Then Program Reads Back".
#
# WHAT THIS DOES NOT COVER (stays the HIL bench's job — never a substitute):
#   - Flash P/E timing and the fetch-stall hazard that makes flash_lp.rs's P/E
#     leaves RAM-resident: this model completes P/E instantly with NO stall, so a
#     RAM-residency regression that would BRICK silicon passes silently here.
#   - CAN bit-timing / 250 kbit/s / sample point / arbitration / bus-off: the
#     model is frame-level, not bit-level.
#   - HOCO clock tolerance, USB PHY/DFU, analog. A green run here complements the
#     bench exit criteria; it does not replace them.
#
# HOST-SIDE CAN E2E (the real can_flash.py) is out of this suite's scope because
# `emulation CreateSocketCANBridge` is Linux-only. Here CAN is driven in-process:
# frames are injected with `sysbus.can0 InjectStandardFrame` (a test hook on the
# model) and replies observed with a native CANTester on the CANHub, so the suite
# runs headless cross-platform. For the full host-tool loop, see the bridge notes
# in renode/bootloader.resc and run can_flash.py against vcan0 on a Linux runner.
#
# Run from the REPO ROOT (paths are ${CURDIR}-relative; Renode's CWD must be the
# repo root and its path must have no spaces/parentheses):
#   cargo build --release -p bootloader
#   <renode-install>/renode-test renode/bootloader.robot

*** Settings ***
Test Setup                      Reset Emulation

*** Variables ***
${GPT_MODEL}          @${CURDIR}/Ra4m1Gpt320.cs
${FACI_MODEL}         @${CURDIR}/Ra4m1FlashLp.cs
${CAN_MODEL}          @${CURDIR}/Ra4m1Can0.cs
${PLATFORM}           @${CURDIR}/ra4m1-uno-r4.repl
${BOOT_ELF}           @${CURDIR}/../target/thumbv7em-none-eabihf/release/bootloader

# Backup-register (VBTBKR) boot-mode handoff words (bootloader/src/main.rs,
# crates/ra4m1-embassy/src/vbtbkr.rs; VBTBKR array base 0x4001E500).
${VBTBKR_DOUBLE_TAP}  0x4001E500
${VBTBKR_CAN_UPDATE}  0x4001E504
${CAN_UPDATE_MAGIC}   0xB007CAFE

# CAN update protocol defaults (crates/boot-logic: dfu_config, protocol).
${CMD_ID}             0x79E
${REPLY_ID}           0x79F
${CMD_HELLO}          0x01
${CMD_SIZE}           0x04
${CMD_ERASE}          0x02
${CMD_DATA}           0x03
${PROTO_VERSION}      0x01

# App slot base (boot_logic::memory_map::APP_BASE) — first erasable app block.
${APP_BASE}           0x8000

*** Keywords ***
Build Bootloader Machine
    # Mirror renode/bootloader.resc: compile the models, load the base platform,
    # then a fragment binding FACI-LP, CAN0 and the VBTBKR backup memory, load the
    # real ELF, and wire CAN0 to a CANHub.
    Execute Command                 include ${GPT_MODEL}
    Execute Command                 include ${FACI_MODEL}
    Execute Command                 include ${CAN_MODEL}
    Execute Command                 mach create "uno-r4-boot"
    Execute Command                 machine LoadPlatformDescription ${PLATFORM}
    Execute Command                 machine LoadPlatformDescriptionFromString "faci: MTD.Ra4m1FlashLp @ sysbus 0x407EC000"
    Execute Command                 machine LoadPlatformDescriptionFromString "can0: CAN.Ra4m1Can0 @ sysbus 0x40050000"
    Execute Command                 machine LoadPlatformDescriptionFromString "vbtbkr_backup: Memory.ArrayMemory @ sysbus 0x4001E500 { size: 0x200 }"
    Execute Command                 sysbus LoadELF ${BOOT_ELF}
    Execute Command                 emulation CreateCANHub "canHub"
    Execute Command                 connector Connect sysbus.can0 canHub

Enter Can Only Update Mode
    # SPEC §2 step 2: the app-requested CAN magic in VBTBKR[4] makes the boot
    # decision enter CAN-only update mode (only can_update spawns — no USB/LED).
    Execute Command                 sysbus WriteDoubleWord ${VBTBKR_CAN_UPDATE} ${CAN_UPDATE_MAGIC}

Read Dword
    [Arguments]                     ${addr}
    ${raw}=                         Execute Command    sysbus ReadDoubleWord ${addr}
    ${val}=                         Convert To Integer    ${raw.strip()}    16
    [Return]                        ${val}

Read Model Uint
    # Read a public uint/int property off a peripheral (e.g. `sysbus.can0
    # SentCount`). Renode's monitor prints it as a 0x-hex string; base 16 handles
    # the 0x prefix. Renode has no `Create CAN Tester`/`Wait For Frame With Id`
    # keyword (v1.16.1), so reply frames are observed through the Ra4m1Can0 test
    # hooks (SentCount / LastSentId) instead — see renode/Ra4m1Can0.cs.
    [Arguments]                     ${expr}
    ${raw}=                         Execute Command    ${expr}
    ${val}=                         Convert To Integer    ${raw.strip()}    16
    [Return]                        ${val}

Inject And Settle
    # Inject one CAN frame and give the executor bounded virtual time to drain the
    # RX FIFO, run the protocol engine, and transmit any reply (deterministic; TX
    # completes instantly in the frame-level model).
    [Arguments]                     ${id}  ${b0}  ${b1}  ${b2}  ${b3}  ${b4}  ${b5}  ${b6}  ${b7}  ${settle}=0.4
    Inject Frame                    ${id}  ${b0}  ${b1}  ${b2}  ${b3}  ${b4}  ${b5}  ${b6}  ${b7}
    Execute Command                 emulation RunFor "${settle}"

Read RTT WrOff
    # defmt-rtt up-channel 0 bytes-written: _SEGGER_RTT control block + 0x24.
    ${sym}=                         Execute Command    sysbus GetSymbolAddress "_SEGGER_RTT"
    ${base}=                        Convert To Integer    ${sym.strip()}    16
    ${addr}=                        Evaluate    hex(${base} + 0x24)
    ${val}=                         Read Dword    ${addr}
    [Return]                        ${val}

Inject Frame
    [Arguments]                     ${id}  ${b0}  ${b1}  ${b2}  ${b3}  ${b4}  ${b5}  ${b6}  ${b7}
    Execute Command                 sysbus.can0 InjectStandardFrame ${id} ${b0} ${b1} ${b2} ${b3} ${b4} ${b5} ${b6} ${b7}

*** Test Cases ***
Bootloader Boots To First Log Line
    [Documentation]                 Core + memory + vector table + LoadELF + the
    ...                             CGC-ready shim: the real ELF boots and defmt-rtt
    ...                             emits the S0 banner, so the up-channel WrOff grows
    ...                             from 0. Mirrors bench S0.
    Build Bootloader Machine
    Execute Command                 emulation RunFor "0.05"
    ${wroff}=                       Read RTT WrOff
    Should Be True                  ${wroff} > 0
    ...                             bootloader emitted no defmt output — it wedged before the S0 banner

VBTBKR Magic Survives CPU Reset And Is Consumed
    [Documentation]                 The boot-mode handoff word persists across a
    ...                             soft (CPU) reset — the survive-NVIC_SystemReset
    ...                             semantic the boot decision depends on — then the
    ...                             boot decision reads and clears it. Mirrors bench S5.
    Build Bootloader Machine
    Enter Can Only Update Mode
    ${before}=                      Read Dword    ${VBTBKR_CAN_UPDATE}
    Should Be Equal As Integers     ${before}    ${CAN_UPDATE_MAGIC}
    ...                             magic did not land in the backup register memory
    Execute Command                 sysbus.cpu Reset
    ${survived}=                    Read Dword    ${VBTBKR_CAN_UPDATE}
    Should Be Equal As Integers     ${survived}    ${CAN_UPDATE_MAGIC}
    ...                             backup register did NOT survive sysbus.cpu Reset
    Execute Command                 emulation RunFor "0.3"
    ${after}=                       Read Dword    ${VBTBKR_CAN_UPDATE}
    Should Be Equal As Integers     ${after}    0
    ...                             boot decision did not consume (clear) the CAN-update magic

CAN Update HELLO Gets A Reply
    [Documentation]                 CAN-only mode -> CAN0 init (mode sequencing) ->
    ...                             RX FIFO drain -> protocol engine -> mailbox-0 TX:
    ...                             a HELLO injected on the bus draws a reply frame on
    ...                             the reply ID. Mirrors bench M1 HELLO/--info.
    Build Bootloader Machine
    Enter Can Only Update Mode
    Execute Command                 emulation RunFor "0.25"
    Inject And Settle               ${CMD_ID}  ${CMD_HELLO}  ${PROTO_VERSION}  0  0  0  0  0  0
    ${replies}=                     Read Model Uint    sysbus.can0 SentCount
    Should Be True                  ${replies} >= 1
    ...                             no reply frame was transmitted on the reply ID for HELLO
    ${id}=                          Read Model Uint    sysbus.can0 LastSentId
    Should Be Equal As Integers     ${id}    ${REPLY_ID}
    ...                             reply used CAN ID ${id}, expected the reply ID ${REPLY_ID}

FACI Erase Then Program Reads Back
    [Documentation]                 Drives the real flash driver through the CAN
    ...                             update protocol: ERASE blanks the app slot (block
    ...                             reads 0xFFFFFFFF) and a DATA window programs 8
    ...                             bytes that read back through the FACI model into
    ...                             code flash. Mirrors bench S3. Does NOT test P/E
    ...                             timing or RAM-residency (see suite header).
    Build Bootloader Machine
    Enter Can Only Update Mode
    Execute Command                 emulation RunFor "0.25"
    # HELLO -> Greeted, SIZE(8) -> Sized. Each reply is observed via SentCount
    # growing (Renode v1.16.1 has no Wait For Frame keyword — see Read Model Uint).
    Inject And Settle               ${CMD_ID}  ${CMD_HELLO}  ${PROTO_VERSION}  0  0  0  0  0  0
    Inject And Settle               ${CMD_ID}  ${CMD_SIZE}  0  0x08  0  0  0  0  0
    # ERASE -> the driver erases the metadata block + all 111 app blocks.
    Inject And Settle               ${CMD_ID}  ${CMD_ERASE}  0  0  0  0  0  0  0  settle=1.0
    ${erased}=                      Read Dword    ${APP_BASE}
    Should Be Equal As Integers     ${erased}    0xFFFFFFFF
    ...                             FACI ERASE did not blank the app slot's first block
    # Two DATA frames complete the 8-byte window -> ProgramBlock 0 at APP_BASE.
    Inject And Settle               ${CMD_ID}  ${CMD_DATA}  0x00  0x11  0x22  0x33  0x44  0x55  0x66
    Inject And Settle               ${CMD_ID}  ${CMD_DATA}  0x01  0x77  0x88  0  0  0  0
    ${lo}=                          Read Dword    ${APP_BASE}
    ${hi}=                          Read Dword    0x8004
    Should Be Equal As Integers     ${lo}    0x44332211
    ...                             programmed low word read back wrong through the FACI model
    Should Be Equal As Integers     ${hi}    0x88776655
    ...                             programmed high word read back wrong through the FACI model
