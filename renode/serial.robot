*** Comments ***
# M2.5 serial-bus suite for ra4m1-hal under Renode v1.16.1: SCI UART, RIIC I2C,
# and RSPI SPI.  Each case boots a REAL release example ELF (the artifact `cargo
# build --release --example` produces) on the UNO R4 Minima platform, adds the
# per-bus peripherals/wiring the driver needs, and asserts the bus transaction
# the firmware drives.
#
# WHAT THIS VALIDATES: driver REGISTER-LOGIC (the SPCR/SPSR/SPDR, SSR/TDR/RDR,
# ICCR/ICDRT/ICDRR handshakes and the START/addr/data/STOP sequencing), NOT
# silicon facts.  Bit rates, SCL/SCK timing, rise/fall, clock tolerance, SSL/CS,
# analog, and the ICU EVENT NUMBERS are all either faked or unverified here — a
# model coded from the same FSP assumptions as the driver would pass circularly
# (renode/TODO.md).  These stay the HIL bench's job.
#
# STOCK-MODEL vs DRIVER — verified against the pinned model/platform sources:
#   * SCI (sci.rs): Renode's stock `SCI.RenesasRA6M5_SCI` register LAYOUT MATCHES
#     the driver (SMR@0, BRR@1, SCR@2, TDR@3, SSR@4 with TDRE@7/RDRF@6/TEND@2,
#     RDR@5, SEMR@7) — no new C# needed.  BUT the stock `renesas-r7fa4m1a.repl`
#     wires SCI2's RXI/TXI/TEI to ICU events 0xA3/0xA4/0xA5, whereas sci.rs
#     programs 0x97/0x98/0x99.  Both are unverified FSP orderings (BENCH-VERIFY);
#     they DIVERGE.  The fragment re-points SCI2 to the driver's numbers so the
#     async echo's RXI actually reaches IEL2.  This validates the driver's RX/TX
#     register handshake under its OWN event-number assumption, not the numbers.
#   * IIC (i2c.rs): stock `I2C.RenesasRA_IIC` layout MATCHES, and its repl IRQ
#     wiring 0x035..0x038 MATCHES i2c.rs — no rewire needed.  A `MockI2CSlave`
#     register-file device is added at 0x50 so the master has something to talk to.
#   * SPI (spi.rs): the stock repl models SPI0 only as an absorbing Tag, so a full
#     custom `SPI.Ra4m1Spi` (internal loopback) is provided.
#
# ELF-COVERAGE NOTES (honesty about what each example ELF actually drives):
#   * uart example: async echo — exercises SSR/TDR/RDR + the RXI-driven RX path.
#   * spi example : `transfer_in_place` of a rolling pattern once/second —
#     exercises the full-duplex SPDR write+read handshake.  Loopback data
#     integrity (rx == tx) is NOT observable from the firmware side (defmt is not
#     decoded in Renode); the model instead proves every written frame was read
#     back (ReadCount == TransferCount).
#   * i2c example: a bus SCAN (zero-length writes) — exercises START/addr/ACK/STOP
#     against the mock slave (Case "I2c Scan ...").  NO current ra4m1-hal example
#     ELF performs a `write_read`, so the register READ-BACK value is asserted by
#     driving one write_read through the stock RIIC engine at the register level
#     (Case "I2c Register Read Back ...").  That case validates the MockI2CSlave +
#     stock RIIC model, not the Rust driver's read path.  ELF DEPENDENCY: a
#     `write_read` example (e.g. examples/i2c_regread) would let the Rust driver
#     drive the read-back end-to-end.
#
# Run from the REPO ROOT (Renode cannot tokenize spaces/parens in @paths, so all
# paths are repo-relative and Renode's CWD must be the repo root):
#   <renode-install>/renode-test renode/serial.robot

*** Settings ***
Test Teardown       Reset Emulation

*** Variables ***
# Paths are absolute via ${CURDIR} (this .robot's directory) so they resolve
# regardless of Renode's own working directory under renode-test.  ${CURDIR} is
# <repo>/renode; the ELFs live under <repo>/target.
${GPT_MODEL}          @${CURDIR}/Ra4m1Gpt320.cs
${SPI_MODEL}          @${CURDIR}/Ra4m1Spi.cs
${MOCK_MODEL}         @${CURDIR}/MockI2CSlave.cs
${PLATFORM}           @${CURDIR}/ra4m1-uno-r4.repl
${UART_ELF}           @${CURDIR}/../target/thumbv7em-none-eabihf/release/examples/uart
${SPI_ELF}            @${CURDIR}/../target/thumbv7em-none-eabihf/release/examples/spi
${I2C_ELF}            @${CURDIR}/../target/thumbv7em-none-eabihf/release/examples/i2c
# One repl indent level for the inline platform fragment (leading literal spaces
# are stripped by Robot, so the indent must come from a variable).
${INDENT}             ${SPACE}${SPACE}${SPACE}${SPACE}
# The mock I2C slave's 7-bit address and the register it serves for read-back.
${MOCK_ADDR}          0x50
${READBACK_REG}       0x10
${READBACK_VAL}       0xAB

*** Keywords ***
Build Machine
    [Arguments]                     ${elf}
    # Compile the custom models (idempotent per machine, mirroring smoke.robot).
    Execute Command                 include ${GPT_MODEL}
    Execute Command                 include ${SPI_MODEL}
    Execute Command                 include ${MOCK_MODEL}
    Execute Command                 mach create "uno-r4-serial"
    Execute Command                 machine LoadPlatformDescription ${PLATFORM}
    Load Serial Peripherals
    Execute Command                 sysbus LoadELF ${elf}

Load Serial Peripherals
    ${frag}=                        Catenate    SEPARATOR=${\n}
    ...                             sci2:
    ...                             ${INDENT}ReceiveIRQ -> icu@0x97
    ...                             ${INDENT}TransmitIRQ -> icu@0x98
    ...                             ${INDENT}TransmitEndIRQ -> icu@0x99
    ...                             ${EMPTY}
    ...                             spi0: SPI.Ra4m1Spi @ sysbus 0x40072000
    ...                             ${INDENT}ReceiveIRQ -> icu@0xA1
    ...                             ${INDENT}TransmitIRQ -> icu@0xA2
    ...                             ${EMPTY}
    ...                             mockSlave: I2C.MockI2CSlave @ iic0 ${MOCK_ADDR}
    Execute Command                 machine LoadPlatformDescriptionFromString "${frag}"

Read Uint
    # Read a numeric monitor property/return; Convert To Integer with no base
    # auto-detects a decimal or 0x-hex string (as smoke.robot relies on).
    [Arguments]                     ${expr}
    ${raw}=                         Execute Command    ${expr}
    ${val}=                         Convert To Integer    ${raw.strip()}
    [Return]                        ${val}

*** Test Cases ***
Uart Echoes A Line Under The Executor
    [Documentation]                 SCI2 async UART echo: a line written to the device is
    ...                             received (RXI -> IEL2 -> waker) and written back, all via
    ...                             embedded_io_async with no busy-wait. Validates the SSR/TDR/RDR
    ...                             register handshake + the RXI-driven RX path (stock SCI layout;
    ...                             SCI2 events re-pointed to the driver's 0x97/0x98/0x99).
    Build Machine                   ${UART_ELF}
    Create Terminal Tester          sysbus.sci2    timeout=8
    Start Emulation
    # The firmware raw-echoes every received byte (uart.rs echo task), so the host
    # must send the newline (0x0A) that `Wait For Line On Uart` treats as the line
    # terminator. `Write Line To Uart` appends a CR (0x0D), NOT an LF, and the
    # tester does not treat a bare CR as end-of-line — so the echoed "PING\r" is
    # never seen as a complete line. Embed the LF in the content (and drop the
    # tester's own echo-wait, which our device satisfies via the explicit LF).
    Write Line To Uart              PING${\n}    waitForEcho=false
    Wait For Line On Uart           PING
    Write Line To Uart              RA4M1${\n}    waitForEcho=false
    Wait For Line On Uart           RA4M1

Spi Loopback Transfers Run Under The Executor
    [Documentation]                 SPI0 master full-duplex loopback: the firmware
    ...                             transfer_in_place's a rolling pattern once/second. Validates
    ...                             the SPCR/SPSR/SPDR per-frame write+read handshake and that the
    ...                             GPT time base keeps the executor re-running it. Data integrity
    ...                             is shown as "every written frame was read back".
    Build Machine                   ${SPI_ELF}
    # First transfer (4 frames) happens near boot, before the first 1 s Timer.
    Execute Command                 emulation RunFor "1.5"
    ${frames1}=                     Read Uint    sysbus.spi0 TransferCount
    ${reads1}=                      Read Uint    sysbus.spi0 ReadCount
    ${first}=                       Read Uint    sysbus.spi0 FirstByte
    Should Be True                  ${frames1} >= 4
    ...                             SPI transfer never ran: TransferCount=${frames1} (expected >= 4)
    Should Be Equal As Integers     ${reads1}    ${frames1}
    ...                             read phase incomplete: ReadCount=${reads1} != TransferCount=${frames1}
    Should Be Equal As Integers     ${first}    165
    ...                             first frame was ${first} (decimal), expected 165 == 0xA5 (the example's seed)
    # Prove the executor + GPT keep driving transfers across virtual seconds.
    Execute Command                 emulation RunFor "3.0"
    ${frames2}=                     Read Uint    sysbus.spi0 TransferCount
    Should Be True                  ${frames2} > ${frames1}
    ...                             SPI exercise task stalled: TransferCount stuck at ${frames1}

I2c Scan Reaches The Mock Slave
    [Documentation]                 RIIC master bus scan: the firmware probes 0x08..0x77 with
    ...                             zero-length writes. Validates the driver's START/address/ACK/
    ...                             STOP handshake against a real II2CPeripheral (stock RIIC layout
    ...                             + wiring both MATCH i2c.rs) and that a NACK at absent addresses
    ...                             does not wedge the scan.
    Build Machine                   ${I2C_ELF}
    Execute Command                 emulation RunFor "3.0"
    ${addressed}=                   Read Uint    sysbus.iic0.mockSlave TransactionCount
    Should Be True                  ${addressed} >= 1
    ...                             master never completed a transaction against the 0x50 slave

I2c Register Read Back Returns The Stored Value
    [Documentation]                 Register read-back over the stock RIIC engine + MockI2CSlave.
    ...                             No ra4m1-hal example ELF drives write_read, so this drives one
    ...                             at the register level (mirroring i2c.rs run_ops for
    ...                             [Write([reg]), Read(1)]) and asserts the stored byte comes back.
    ...                             Validates the mock's register-file + pointer retention across a
    ...                             repeated-START, NOT the Rust driver's read path (ELF dependency:
    ...                             an examples/i2c_regread bin).
    Build Machine                   ${I2C_ELF}
    # Let the firmware bring up iic0 (ICE=1, bit rate, reset released) and finish
    # its scan; iic0 is then initialised and idle for manual driving.
    Execute Command                 emulation RunFor "3.0"
    Execute Command                 sysbus.iic0.mockSlave SetRegister ${READBACK_REG} ${READBACK_VAL}
    # START, then address (write): 0x50 << 1 = 0xA0.
    Execute Command                 sysbus.iic0 WriteByte 0x01 0x02
    Execute Command                 sysbus.iic0 WriteByte 0x12 0xA0
    # The RIIC schedules the address write 100 us out; let it resolve.
    Execute Command                 emulation RunFor "0.001"
    # Register pointer (data byte of the write phase).
    Execute Command                 sysbus.iic0 WriteByte 0x12 ${READBACK_REG}
    # Repeated-START (flushes [reg] to the slave, keeps the pointer), then
    # address (read): 0x50 << 1 | 1 = 0xA1.
    Execute Command                 sysbus.iic0 WriteByte 0x01 0x04
    Execute Command                 sysbus.iic0 WriteByte 0x12 0xA1
    Execute Command                 emulation RunFor "0.001"
    # Dummy ICDRR read kicks off reception (the model enqueues a 0x0 for it),
    # then the real ICDRR read returns register[0x10].
    Execute Command                 sysbus.iic0 ReadByte 0x13
    ${value}=                       Read Uint    sysbus.iic0 ReadByte 0x13
    Should Be Equal As Integers     ${value}    171
    ...                             read-back was ${value} (decimal), expected 171 == 0xAB from register 0x10
