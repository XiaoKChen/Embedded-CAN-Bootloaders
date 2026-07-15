# Custom Bootloader for Arduino UNO R4 Minima with Flash-over-CAN (D4/D5)

Research document — 2026-07-14.
Goal: a bootloader that behaves like the stock UNO R4 Minima bootloader but adds **one** feature: flashing an application `.bin` over CAN bus on pins D4/D5.

Everything below marked **[VERIFIED]** was confirmed against primary sources (repo source code, FSP headers, or the shipped bootloader hex — commands and file paths given). Items marked **UNVERIFIED** could not be confirmed and must be checked against the RA4M1 Hardware User's Manual (R01UM0007) or on hardware before relying on them.

---

## 1. Executive Summary & Recommendation

The UNO R4 Minima ships with Arduino's TinyUSB-based **USB DFU bootloader** (repo: `arduino/arduino-renesas-bootloader`), occupying flash `0x0000–0x3FFF` (16 KB reserved; ~12.4 KB actually used). The application (sketch) lives at **`0x4000`** with its vector table at that address. The RA4M1 has exactly one classic CAN 2.0B controller (**CAN0**), which the Minima routes to **D4 = CTX0 = P103** and **D5 = CRX0 = P102**. The board has **no CAN transceiver** — an external 5 V transceiver (TJA1050 / MCP2551 / MCP2562) is mandatory.

**Recommendation: Strategy (a) — fork `arduino-renesas-bootloader`, keep the USB DFU path intact, and add a polled register-level CAN0 update path, staying inside the existing 16 KB / app-at-0x4000 memory map.**

Why:

- It literally satisfies "behaves like the stock bootloader + one feature": the Arduino IDE upload workflow (dfu-util → DFU detach → DFU download at 0x4000) keeps working unchanged, because the memory map and USB descriptors are untouched.
- Size headroom exists but is tight: stock uses **12,424 bytes** of the 16,384-byte region (measured from `dfu_minima.hex`), leaving **~3.9 KB** for a polled CAN driver + chunk protocol + CRC32 under `-Os -flto`. This is feasible but must be gated with a size check from day one (see §9 risk R1).
- Brick risk is low in absolute terms: the RA4M1's **mask-ROM boot mode** (BOOT pad → GND, "Renesas RA USB Boot", flashed with `rfp-cli`) and the SWD pads (pyOCD/J-Link) are always available regardless of what is in flash. The device is unbrickable at the chip level.
- Fallback if the 16 KB budget is blown or TinyUSB proves fragile: Strategy (b), a second-stage CAN bootloader at 0x4000 (the Arduino core's linker script even has a dedicated `.sketch_boot` section for exactly this pattern). Strategy (c) (CAN-only, drop USB) is right for headless production nodes, not for a board developed with the Arduino IDE.

Protocol recommendation: do **not** port OpenBLT (GPLv3, ~5–20 KB footprint, interrupt-oriented ports). Use a minimal custom chunked protocol over two fixed 11-bit CAN IDs — ISO-TP-inspired windowed transfer, 7 data bytes + 1 sequence byte per frame, ACK-paced around flash writes, CRC32-validated image with a metadata block written last. Host side: a ~150-line `python-can` script on any SLCAN/candleLight adapter.

---

## 2. Stock Bootloader Findings

### 2.1 Identity, location, size

| Fact | Value | Status |
|---|---|---|
| Source repo | <https://github.com/arduino/arduino-renesas-bootloader> ("A tinyusb based project implementing the bootloader for Uno R4 and Portenta C33") | [VERIFIED] |
| Shipped binary | `bootloaders/UNO_R4/dfu_minima.hex` in <https://github.com/arduino/ArduinoCore-renesas> | [VERIFIED] |
| License of bootloader repo | **No LICENSE file** — GitHub API reports `license: null`. Legal status of forking/redistribution is unclear; ArduinoCore-renesas (which ships the hex) is **MIT** per GitHub API. Ask Arduino or treat conservatively. | [VERIFIED that no license file exists] |
| Flash footprint | Contiguous `0x0000–0x3088` = **12,424 bytes (~12.1 KB)**, plus 28 bytes of ID-code/config data at `0x01010018`. Measured by parsing `dfu_minima.hex`. | [VERIFIED] |
| Reserved bootloader region | `0x0000–0x3FFF` (16 KB): app linker sets `FLASH_IMAGE_START = 0x4000`; bootloader `src/flash.h` sets `SKETCH_FLASH_OFFSET = (16 * 1024)` for flash-LP parts | [VERIFIED] |
| USB identity | `USB_PID=0x0369`, board name "UNO R4 Minima" (`Makefile.minima`) | [VERIFIED] |
| Protocol | USB DFU 1.1 (TinyUSB `tud_dfu`, pinned tinyusb v0.17.0 + one Arduino patch). DFU flash descriptor: `@CodeFlash /0x00000000/8*2Ka,120*2Kg` — i.e. 8×2 KB read-only (bootloader) + 120×2 KB erase/write (app), matching 128×2 KB = 256 KB total | [VERIFIED] |
| Build | `./compile-bootloader.sh R4-minima` (or `make -f Makefile.minima` with tinyusb checked out); `-Os -flto -ffunction-sections -fdata-sections -Wl,--gc-sections`; uses FSP `r_flash_lp`, `r_ioport`, `r_gpt` sources via tinyusb's RA family BSP | [VERIFIED] |

### 2.2 Bootloader entry (how it decides DFU vs jump) — from `src/main.c` [VERIFIED]

- **Double-tap reset**, communicated through a battery-backup register:
  - `BOOT_DOUBLE_TAP_DATA` = `*(volatile uint32_t *)&R_SYSTEM->VBTBKR[0]` (on Cortex-M23 parts a fixed RAM word `0x20007FF0` is used instead; the RA4M1/Minima uses **VBTBKR[0]**).
  - Magic value: **`0x07738135`** (`DOUBLE_TAP_MAGIC`).
  - Writing VBTBKR requires unlocking the register-protection register: `R_SYSTEM->PRCR = 0xA502` (key `0xA500` | PRC1), then re-lock with `0xA500`.
- Boot flow in `main()`:
  1. If `BOOT_DOUBLE_TAP_DATA == DOUBLE_TAP_MAGIC` → clear it → **stay in bootloader** (DFU).
  2. Else, if the reset was *not* power-on (`!R_SYSTEM->RSTSR0_b.PORF`), write the magic, busy-wait **500 ms**, then clear it. A second reset tap inside that window re-enters with the magic set → DFU.
  3. Validate the app: `app_valid = ((*(uint32_t *)0x4000) & 0xFF000000) == 0x20000000` — i.e. *only* checks that the app's initial stack pointer word points into SRAM. **No CRC, no size check.**
  4. If valid → `boot5(0x4000)`.
- **IDE upload trigger:** `boards.txt` sets `minima.upload.use_1200bps_touch=false`. Instead, the running sketch exposes a **DFU runtime interface** (TinyUSB); `dfu-util` sends DFU_DETACH and the core's handler `tud_dfu_runtime_reboot_to_dfu_cb()` (in `cores/arduino/usb/SerialUSB.cpp`) writes `DOUBLE_TAP_MAGIC` to VBTBKR[0] and resets via a 16k-cycle watchdog (or `NVIC_SystemReset()` if `NO_WATCHDOG`). A 1200-bps-touch fallback also exists in the core (`CheckSerialReset()`).

### 2.3 Jump-to-application sequence (`boot5()`) [VERIFIED]

```c
R_BSP_MODULE_STOP(FSP_IP_USBFS, 0);          // deinit USB (module-stop)
R_MPU_SPMON->SP[0].CTL = 0;                  // disable MSP monitoring
__disable_irq(); __DSB(); __ISB();
SysTick->CTRL = 0;                           // stop SysTick
SCB->VTOR = 0x4000;                          // bootloader sets VTOR, not the app
__set_MSP(*(uint32_t *)0x4000);
((void (*)(void))(*(uint32_t *)0x4004))();   // app reset handler
```

Note: the stock bootloader jumps **with IRQs disabled**; the Arduino core re-enables them. It sets VTOR *before* the jump (the "bootloader sets VTOR" variant per our `bootloader.md` rule — document this choice in `memory_map.h`).

### 2.4 Flash writing in the stock bootloader [VERIFIED]

Uses FSP **`r_flash_lp`**: `R_FLASH_LP_Open/Reset` at bootloader start, then per DFU block `R_FLASH_LP_Erase(ctrl, 0x4000 + block*4096, 2)` (CFG_TUD_DFU_XFER_BUFSIZE-sized chunks, `PROGRAM_BLOCK_SIZE = 2*1024`) and `R_FLASH_LP_Write`. No read-back verify beyond what the FCU reports.

---

## 3. RA4M1 Hardware Facts

MCU on the Minima: **R7FA4M1AB3CFM#AA0** (Cortex-M4F, 48 MHz, 64-pin LQFP), operating at **5 V** logic on this board.

### 3.1 Memory map (from `variants/MINIMA/memory_regions.ld` [VERIFIED])

| Region | Origin | Length |
|---|---|---|
| Code flash | `0x00000000` | `0x40000` (256 KB) |
| SRAM | `0x20000000` | `0x8000` (32 KB) |
| Data flash | `0x40100000` | `0x2000` (8 KB) |
| ID code (OSIS) | `0x01010018` | `0x20` |
| App image start | **`0x4000`** (`FLASH_IMAGE_START`) | — |

### 3.2 Flash erase/write granularity (from FSP `ra/fsp/src/bsp/mcu/ra4m1/bsp_feature.h` [VERIFIED])

| Parameter | Value |
|---|---|
| Flash driver | `r_flash_lp`, `BSP_FEATURE_FLASH_LP_VERSION = 3` (MF3 flash macro) |
| Code flash **write unit** | **8 bytes** (`CF_WRITE_SIZE = 0x08`) — writes must be 8-byte aligned multiples |
| Code flash **erase block** | **2048 bytes** (`CF_BLOCK_SIZE = 0x0800`) → 128 blocks total |
| Data flash write unit | **1 byte** (`DF_WRITE_SIZE = 0x01`) |
| Data flash erase block | **1024 bytes** (`DF_BLOCK_SIZE = 0x0400`) → 8 blocks |

Erase/program *times* per block: not confirmed here — take tPBW/tBE from the RA4M1 datasheet electrical characteristics (**UNVERIFIED**, expect order-of-ms per 2 KB block; measure on hardware).

### 3.3 Programming code flash while executing from it

Per FSP `r_flash_lp` docs [VERIFIED, docs]:

- "**Code flash cannot be accessed while writing, erasing or blank checking code flash**" (RA4M1 has no dual-bank/bank-programming). The flash-programming routines must execute **from RAM** — the FSP driver handles this placement itself (see Renesas KB "Running the FSP Flash Driver from RAM").
- **Interrupts must be disabled** during code-flash P/E (a vector fetch from flash during P/E faults). Fine for us: the bootloader is poll-only by project rule.
- Flash P/E clock: **FCLK ≥ 4 MHz** required. Arduino clock tree gives FCLK = 24 MHz (§3.5). `R_FLASH_LP` must be told the FCLK frequency (FSP does this via BSP clock config).
- Data flash *can* be programmed while executing from code flash (BGO), which is why the metadata-in-data-flash option in §6.4 works.

### 3.4 Option-setting memory (OFS) and ID code — the bootloader owns them

- On the RA4M1, **OFS0 is at flash address `0x0400`, OFS1 at `0x0404`**, with the Security-MPU settings following at 0x408–0x43F. Evidence: the Minima `fsp.ld` comment "*ROM registers are at address 0x400*" [VERIFIED], the linker's `.option_setting_ofs0/ofs1` sections, and the stock hex contents at 0x400 [VERIFIED]:
  - `OFS0 = 0xFFFFFFFF` (IWDT/WDT auto-start disabled)
  - `OFS1 = 0xFFFFCEDF` (LVD/HOCO options)
  - `0x408…` = `0x00FFFFFC, 0x00FFFFFF, …` (Security MPU defaults)
- These addresses fall inside the **bootloader's** flash region — the application at 0x4000 physically cannot set them. **A custom bootloader image must contain correct OFS values** (safest: byte-identical to the stock hex values above). Getting OFS0 wrong can auto-start the IWDT and reset a bootloader that doesn't feed it.
- **ID code (OSIS)** at `0x01010018` (0x20-byte region): controls debugger/serial-programmer authentication. Stock hex programs this region (28 data bytes); keep it **all `0xFF` = protection disabled** or you can lock yourself out of SWD/boot-mode reflashing. `ID_CODE` region exists in `fsp.ld` for this purpose. [VERIFIED that the region exists and stock programs it; byte values assumed 0xFF — **UNVERIFIED**, dump before reusing.]
- Exact OFS field meanings: RA4M1 HW manual "Option-Setting Memory" chapter (**UNVERIFIED** details here).

### 3.5 Clock tree as configured by Arduino (from `variants/MINIMA/includes/ra_gen/bsp_clock_cfg.h` [VERIFIED])

- **No external crystal** (`BSP_CFG_XTAL_HZ = 0`). System clock source = **HOCO 48 MHz** (internal oscillator).
- ICLK = 48 MHz, **PCLKA = 48 MHz**, **PCLKB = 24 MHz** (div 2), **FCLK = 24 MHz** (div 2).
- FSP constraint for CAN on RA4M1: **PCLKA:PCLKB must be 2:1** — already satisfied. [VERIFIED, FSP r_can docs]
- Consequence: CAN bit timing derives from the **HOCO internal oscillator**. HOCO trimmed accuracy is on the order of ±1 % (exact spec **UNVERIFIED** — check RA4M1 datasheet). Classic CAN tolerates roughly ≤1.5 % total clock error depending on bit timing; Arduino ships a working CAN library on this exact config, so it works in practice, but prefer 250–500 kbit/s and a high sample point (§5.3) and validate on hardware over temperature.

### 3.6 Backup registers for handoff

`R_SYSTEM->VBTBKR[]` (VBATT backup registers) survive resets and are already used by the ecosystem: **VBTBKR[0]** carries the double-tap/DFU magic. Use a *different* index (e.g. VBTBKR[4..7]) for the CAN-update-request magic and boot counter, or a `.noinit` SRAM word. Number of VBTBKR bytes on RA4M1: **UNVERIFIED** (check HW manual; at least several words exist since index 0 is in production use). PRCR PRC1 unlock required for writes (§2.2).

---

## 4. CAN on D4/D5 — Pin Mapping and Peripheral

### 4.1 Pin mapping [VERIFIED from ArduinoCore-renesas]

`variants/MINIMA/pins_arduino.h`:

```c
#define PIN_CAN0_TX (4)
#define PIN_CAN0_RX (5)
#define PIN_CAN0_STBY (-1)
```

`variants/MINIMA/variant.cpp` + `pinmux.inc`:

- **D4 → `BSP_IO_PORT_01_PIN_03` (P103) → CAN0 CTX** (`P103[] = { … PIN_CAN_TX|CHANNEL_0 … }`)
- **D5 → `BSP_IO_PORT_01_PIN_02` (P102) → CAN0 CRX** (`P102[] = { … PIN_CAN_RX|CHANNEL_0 … }`)

Pin function select: route P103/P102 to the CAN peripheral via the PFS registers (IOPORT peripheral function; FSP `IOPORT_CFG_PERIPHERAL_PIN` with the CAN PSEL code — see `R_PFS` PSEL value for CAN0 in the RA4M1 HW manual, exact PSEL nibble **UNVERIFIED** here; FSP's `r_ioport` + the Arduino `cfg_pins()` code is the working reference).

### 4.2 Peripheral capabilities

- RA4M1 has **one classic CAN 2.0B channel (CAN0)** — `BSP_FEATURE_CAN_NUM_CHANNELS = 1` [VERIFIED, bsp_feature.h]. **No CAN FD.**
- **32 mailboxes**; optional FIFO mode turns mailboxes 24–27 into a 4-deep TX FIFO and 28–31 into a 4-deep **RX FIFO**. Max 1 Mbit/s. [VERIFIED, FSP r_can docs]
- Clock source: **PCLKB** or CANMCLK (main-osc pin — not available on the Minima since there is no crystal). Arduino's own `R7FA4M1_CAN.cpp` uses `CAN_CLOCK_SOURCE_PCLKB` with `F_CAN_CLK = 24 MHz`. [VERIFIED]

### 4.3 No transceiver on board — external hardware required

The Minima exposes CAN0 as logic-level CTX/CRX only. You must wire an external transceiver between D4/D5 and the bus (Arduino's official CAN tutorial states this):

- The RA4M1 runs at **5 V** on this board → use a 5 V transceiver: **TJA1050**, **MCP2551**, or **MCP2562** (MCP2562's VIO pin makes split-rail designs easy). The popular SN65HVD230 is a **3.3 V** part — do not use it directly on 5 V logic.
- Wiring: D4→TXD, D5→RXD, 5V→VCC, GND→GND, CANH/CANL to the bus, 120 Ω termination at both bus ends.

### 4.4 Polled (no-interrupt) CAN driver

FSP's `r_can` is **interrupt-driven by design** — "a user-callback function must be defined that the driver will invoke when transmit, receive or error interrupts are received", and `R_CAN_Read()` is not supported. It cannot be used as-is under our "poll, no interrupts" rule.

Plan: a small **register-level polled driver** (~200–300 lines of C11) against `R_CAN0` (CMSIS device header `R7FA4M1AB.h`, vendored in FSP / in the tinyusb RA BSP). Use FSP's `r_can.c` source and the RA4M1 HW manual CAN chapter as the register reference. Outline:

1. Cancel module-stop for CAN0 (`R_MSTP`), configure P102/P103 PFS.
2. Enter reset mode → set `BCR` (clock source PCLKB, prescaler/TSEG per §5.3), mailbox mode or RX-FIFO mode, masks (`MKR`/`MKIVLR`) to accept only the two protocol IDs.
3. Enter operate mode; poll:
   - RX: `NEWDATA` flag in `MCTL_RX[i]` (or RX-FIFO not-empty in `RFCR`), read `MB[i]`, clear flag.
   - TX: write `MB[tx]`, set TX request in `MCTL_TX`, poll `SENTDATA`.
4. Watch `STR`/error counters for bus-off; on bus-off, reinit and keep waiting (fail-safe: never proceed with a half-programmed slot).

Exact register/bit names above are from FSP driver usage; cross-check each against the RA4M1 HW manual CAN chapter during implementation (**register-level details UNVERIFIED at bit level here**).

---

## 5. Proposed Architecture

### 5.1 Memory map (single source of truth: linker script + `memory_map.h`)

| Region | Address range | Size | Contents |
|---|---|---|---|
| Bootloader (immutable at runtime) | `0x0000_0000 – 0x0000_3FFF` | 16 KB (8×2 KB blocks) | Custom bootloader incl. vectors at 0x0, **OFS0/OFS1 at 0x400/0x404**, TinyUSB DFU + polled CAN |
| Application slot | `0x0000_4000 – 0x0003_F7FF` | 238 KB (119×2 KB blocks) | App vector table at `0x4000` (unchanged vs stock ⇒ Arduino IDE compatibility) |
| Image metadata block | `0x0003_F800 – 0x0003_FFFF` | 2 KB (last block) | Magic, image size, CRC32, written **last** during update |
| SRAM | `0x2000_0000 – 0x2000_7FFF` | 32 KB | Bootloader uses low RAM; `.noinit` handoff word(s) optional |
| VBTBKR[0] | — | 4 B | DFU magic `0x07738135` (stock-compatible) |
| VBTBKR[4] (proposed) | — | 4 B | CAN-update magic + boot counter |
| Data flash | `0x4010_0000 – 0x4010_1FFF` | 8 KB | Untouched by bootloader (EEPROM library territory) — see §6.4 alternative |

The app slot shrinks by one block (238 KB instead of 240 KB) to give the metadata a home that DFU uploads won't normally reach. Keep the DFU descriptor advertising `120*2Kg` for compatibility; a >238 KB sketch would clobber metadata and simply be treated as "unvalidated by CAN path" (see validation policy below).

### 5.2 Boot decision (validate → decide → jump)

```
reset →
  if VBTBKR[0] == 0x07738135          → clear, run USB DFU mode      (stock behavior)
  if VBTBKR[4] == CAN_UPDATE_MAGIC    → clear, run CAN update mode   (app-requested)
  if !PORF: arm 500 ms double-tap window (stock behavior)
  optional: 100–250 ms CAN listen window for a HELLO frame on the boot ID
            (keeps "flash a bricked/blank app over CAN" possible without app cooperation;
             costs boot latency — keep short, or gate on app-invalid only)
  validate app:
     metadata block magic OK && size ≤ slot && CRC32(0x4000, size) OK  → jump
     else if metadata absent/invalid:
        fall back to stock check: (*(u32*)0x4000 & 0xFF000000) == 0x20000000 → jump
        (documented deviation: DFU-uploaded sketches carry no CRC; see below)
     else → stay in bootloader (CAN + DFU both alive), blink error pattern
  jump = stock boot5 sequence (§2.3): module-stop everything we touched (USB, CAN, GPT),
         __disable_irq, SysTick off, VTOR=0x4000, MSP=*(0x4000), jump *(0x4004)
```

Validation-policy note (aligns with `rules/bootloader.md` "never jump to an unvalidated image" while staying Arduino-compatible): after a **DFU** download completes, the bootloader itself computes CRC32 over the received image and writes a fresh metadata block — then *every* boot path is CRC-validated, and the raw-SP fallback only matters for images flashed by tools that bypass both paths (SWD). Watchdog: keep the boot decision under the WDT if enabled, and bound CAN-mode retry loops with the VBTBKR boot counter.

Header-field validation per project rules: magic, `size ≤ 238 KB`, entry point within `0x4000…0x3F7FF`, initial SP within `0x2000_0000…0x2000_8000`, header CRC over the metadata itself. Fail closed. Validation is pure C over a byte buffer → host-testable with Unity/CTest.

### 5.3 CAN bit timing (PCLKB = 24 MHz)

500 kbit/s: prescaler 3 → 8 MHz tq clock → 16 tq/bit: SS=1, TSEG1=11, TSEG2=4, SJW=1 → sample point 75 %. 250 kbit/s: prescaler 6, same segments. (Constraints from FSP/Arduino lib: TQ total 8–25, TSEG1 4–16, TSEG2 2–8, SJW 1–4.) Default to **250 kbit/s** for HOCO-tolerance margin; make it a compile-time constant in `memory_map.h`'s sibling `boot_config.h`.

---

## 6. CAN Update Protocol Sketch

### 6.1 Survey of existing options

| Protocol | Notes | Verdict |
|---|---|---|
| **OpenBLT** (Feaser) | XCP-on-CAN, two CAN IDs (`BOOT_COM_CAN_RX/TX_MSG_ID`), mature host tools (MicroBoot/BootCommander), 5–20 KB target footprint, **GPLv3** (commercial license sold) | Good design reference (seed/key, backdoor entry window); too big for our 3.9 KB budget and GPLv3 is awkward — don't port, borrow ideas |
| **CANopen SDO block transfer** (CiA 301) | Standardized, heavier state machine, needs CANopen stack | Overkill |
| **UDS on ISO-TP** (ISO 14229 / 15765-2) | Automotive standard (services 0x34/0x36/0x37), ISO-TP gives multi-frame + flow control; `python-can-isotp` exists | Right shape, but full UDS is overkill; borrow ISO-TP's block/flow-control idea |
| **Minimal custom chunked protocol** | ~1 KB of target code, trivially host-testable | **Recommended** |

### 6.2 Frame design (classic CAN, 11-bit IDs, DLC 8)

Two fixed IDs (compile-time constants): host→boot **`0x79E`**, boot→host **`0x79F`** (pick any free pair; make them configurable). Byte 0 = command/status, remaining 7 bytes = payload.

Host→target commands:

| Cmd | Name | Payload |
|---|---|---|
| `0x01` | HELLO / enter | protocol version |
| `0x02` | ERASE | none (bootloader erases whole app slot + metadata block) |
| `0x03` | DATA | `seq(1B, mod 256)` + 7 image bytes |
| `0x04` | VERIFY | `size(4B LE)` + `crc32(4B LE)` → spans 2 frames, or use 29-bit ID w/ 8-byte payload; simplest: `size` in VERIFY-A, `crc` in VERIFY-B |
| `0x05` | GO / reset | none |

Target→host replies: `ACK(status, last_seq, detail)` / `NAK(error code, expected_seq)`; HELLO reply carries bootloader version + slot size.

### 6.3 Transfer mechanics (poll-friendly, loss-tolerant)

- **Windowed, ACK-paced:** host sends up to **64 DATA frames** (= 448 bytes < aligned to nothing in particular; choose 292 frames ≈ 2 KB if you prefer block-aligned windows — recommended: **293 frames ≈ one 2 KB flash block**, then ACK). Bootloader buffers into a static 2 KB RAM buffer (8-byte-aligned; pad tail with `0xFF` to the 8-byte write unit), then programs one 2 KB block: erase already done up-front by ERASE, so just `write` + **read-back memcmp verify** (rule: erase → write → read-back). ACK carries the last sequence number; on gap/mismatch the boot NAKs with the expected seq and the host rewinds the window.
- **Why ACK pacing matters:** during each code-flash write (executed from RAM, IRQs off, CPU stalled from flash) the CPU cannot service CAN promptly, and the RX FIFO holds only 4 frames. The host must not send the next window until the ACK arrives. Simple stop-and-wait per 2 KB block costs little: ~9–15 s for a full 238 KB image at 500 kbit/s (≈35 k frames × ~250 µs + 119 block-writes).
- **Order of operations for fail-safety:** ERASE first invalidates the metadata block (erase it *first*), then the app blocks. Metadata (magic+size+CRC) is written **only after** VERIFY recomputes CRC32 over flash and matches. Power loss anywhere → metadata invalid → bootloader stays resident and re-accepts CAN/DFU. No state bricks (single-slot + resident-bootloader recovery model; a dual-slot A/B layout does not fit 256 KB alongside Arduino-sized sketches and would break IDE uploads).
- CRC32: standard IEEE 802.3 (poly 0x04C11DB7 reflected), byte-wise table-less bit loop (~100 bytes of code) to save flash.

### 6.4 Alternative metadata home (considered, not chosen)

Data flash at `0x40100000` is programmable while executing from code flash (1-byte writes, 1 KB erase blocks) and would free the last code-flash block. Rejected as default because the Arduino **EEPROM library** also uses data flash and could clobber the metadata (extent of EEPROM usage: **UNVERIFIED**). If you control the application, reserving data-flash block 7 (`0x40101C00`) is a clean option.

### 6.5 Host-side tool

- **Adapter:** any SLCAN or gs_usb/candleLight USB-CAN dongle (CANable ~$15–25), Kvaser, PCAN, or a second Arduino/socketcan Linux box.
- **Script:** `python-can` (<https://python-can.readthedocs.io>) — open bus at 250/500 k, implement HELLO/ERASE/DATA-window/VERIFY/GO with retries; ~150 lines. Trigger from the app side: an application CAN command (or serial command) writes `CAN_UPDATE_MAGIC` to VBTBKR[4] and resets — mirroring how DFU detach works today.
- Host tests per project rules: the frame codec and the image-validation code are pure C — compile them for x86 and run Unity/CTest against golden vectors (valid image, bad magic, bad CRC, truncated, OOB entry point).

---

## 7. Strategy Comparison (decision record)

| Criterion | (a) Fork stock: DFU + CAN in one bootloader | (b) Keep stock; 2nd-stage CAN loader at 0x4000, app at ~0x8000 | (c) Custom CAN-only bootloader (no USB) |
|---|---|---|---|
| Arduino IDE upload workflow | **Unchanged** (same map, same USB PID/DFU) | Broken by default: IDE writes to 0x4000, overwriting stage-2. Needs combined-image upload or custom board variant with shifted `FLASH_IMAGE_START` (core supports the pattern via `.sketch_boot` section in `fsp.ld`) | Gone — every upload via CAN or SWD |
| Flash overhead | 16 KB total (stock 12.4 KB + ~2–4 KB CAN) — **tight** | 16 KB stock + ~8–16 KB stage-2 = 24–32 KB before app | ~4–6 KB possible (pure C11, no TinyUSB) — could even shrink region later |
| Brick risk | Low: replacing bootloader is the only risky flash op, and ROM boot mode + SWD always recover | **Lowest**: stock bootloader never touched; stage-2 reflashable over USB DFU forever | Low-medium: no USB path, but ROM boot + SWD still recover |
| Complexity | Medium: TinyUSB build system + FSP + new CAN path; size fight | Medium-high: two boot stages, two jump sequences, linker offsets ×2, combined-hex packaging | **Lowest code complexity**, highest workflow friction |
| Keeping "USB DFU compatibility is a big lift?" | Not from scratch — it's already written and building (`compile-bootloader.sh R4-minima`); the lift is integration + size, not reimplementation | n/a (kept for free) | n/a (dropped) |
| Fits project rules (bare-metal C11, CMake, host-testable, poll-only) | Mostly — TinyUSB is vendored C, but build stays Makefile unless ported to CMake; CAN/validation code is ours and testable | Stage-2 can be 100 % our rules (CMake, C11, no USB) | Fully |

**Decision: (a)**, with the size gate and OFS handling from §9. If §9-R1 fires (doesn't fit 16 KB) and you refuse to move `FLASH_IMAGE_START` (which needs a custom boards.txt variant and breaks vanilla IDE), fall back to **(b)**. Choose **(c)** only for deployed CAN nodes where the Arduino IDE no longer matters.

---

## 8. Build & Flash Toolchain

### 8.1 Building

Two-layer approach that satisfies both "behaves like stock" and this repo's CMake rules:

1. **Bootloader binary:** fork `arduino/arduino-renesas-bootloader`. It builds with plain **`arm-none-eabi-gcc` + Make** (no e2 studio): tinyusb v0.17.0 checkout + `0001-fix-arduino-bootloaders.patch` + `make -f Makefile.minima BOARD=uno_r4` — or just `./compile-bootloader.sh R4-minima`. Add: `src/can_boot.c` (polled driver + protocol), `src/image_validate.c` (pure), hook into `main()` before/around `run_bootloader()`. Wrapping this in a CMake `ExternalProject`/custom target keeps the repo's CMake-first rule without porting tinyusb's build.
2. **FSP without e2 studio** (needed if you later go strategy (c) or want full CMake): FSP source is on GitHub (<https://github.com/renesas/fsp> — license reported as `NOASSERTION` by GitHub, i.e. Renesas' own terms: **UNVERIFIED, read `LICENSE` before vendoring**). Minimum vendored set: `ra/fsp/src/bsp` (startup, clocks, `bsp_feature.h`), CMSIS device headers (`R7FA4M1AB.h`), `r_flash_lp`, `r_ioport`. Compile with the repo's `arm-none-eabi.cmake` toolchain file, `-mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=hard -Os -ffunction-sections -fdata-sections`. Known FSP+CMake gotcha: generated `memory_regions.ld`/`fsp.ld` include paths (`-L` the script dir; see renesas/fsp issue #359).
3. **Linker script:** bootloader region `0x0–0x4000`, RAM `0x20000000+0x8000`, plus `.option_setting_ofs0/ofs1` KEEP sections landing at `0x400/0x404` and `.id_code` at `0x01010018` (copy the section plumbing from the Minima `fsp.ld`). Mirror the partition into exactly one `memory_map.h` checked against the `.ld` (project rule).
4. **Host tests:** Unity + CTest targets for `image_validate.c` and the protocol codec, compiled for x86 (project `rules/testing.md`).

### 8.2 Flashing the custom bootloader onto the board

Three independent paths (any one suffices; all coexist):

1. **ROM boot mode + rfp-cli (no extra hardware):** short the **BOOT pad to GND** while resetting/plugging USB → the RA4M1 mask-ROM boots into "Renesas RA USB Boot" (USB CDC). Flash with Renesas Flash Programmer CLI: `rfp-cli -device ra -port <COMx> -p your_bootloader.hex` (keep BOOT–GND shorted until programming starts). This is the officially documented path in the Arduino bootloader README. rfp-cli download: Renesas Flash Programmer page (free, registration).
2. **SWD + pyOCD:** the Minima exposes SWD pads/header; with any CMSIS-DAP (or J-Link) probe: `pyocd flash -e sector -a 0x0 -t r7fa4m1ab your_bootloader.hex` (target name from Zephyr's UNO R4 docs). J-Link device name: `R7FA4M1AB`.
3. **Via the stock bootloader (first time only):** double-tap reset → DFU — but note stock DFU only writes at `0x4000+` (bootloader region is `2Ka` read-only in the descriptor), so this path **cannot** replace the bootloader itself; it's listed to prevent wasted effort. Use 1 or 2.

### 8.3 Unbricking

- The **mask-ROM serial/USB boot mode (path 1)** cannot be erased — the board is always recoverable unless the ID code (OSIS) is programmed to lock it (§3.4: keep `0xFF`).
- Restore factory state: `pyocd flash -e sector -a 0x0 -t r7fa4m1ab dfu_minima.hex` with the hex from `ArduinoCore-renesas/bootloaders/UNO_R4/`, or the same file via rfp-cli in ROM boot mode.

---

## 9. Risks & Open Questions

| # | Risk / open question | Mitigation |
|---|---|---|
| R1 | **16 KB size ceiling**: stock uses 12,424 B; CAN driver + protocol + CRC must fit in ~3.9 KB | Add `arm-none-eabi-size` gate to the build from day one; `-Os -flto` already on; CRC without table; if it doesn't fit → strategy (b) |
| R2 | **HOCO-clocked CAN** (no crystal): exact HOCO tolerance UNVERIFIED; marginal clock → bit errors at 1 Mbit/s or temperature extremes | Default 250 kbit/s, sample point 75–80 %, HIL test across temperature; protocol has per-window CRC-guarded retries anyway |
| R3 | Frames dropped during flash P/E (IRQs off, CPU stalled, 4-deep RX FIFO) | ACK-paced windows sized to one 2 KB block; host never streams past an outstanding window |
| R4 | **OFS0/OFS1/Security-MPU words at 0x400–0x43F** wrong → IWDT surprise resets or worse | Byte-copy stock values (`FFFFFFFF`, `DFCEFFFF`, …) into the custom image; diff `objcopy` output vs `dfu_minima.hex` for 0x400–0x43F before first flash |
| R5 | **ID code (OSIS)** accidentally programmed → SWD/boot-mode lockout | Keep `.id_code` all-0xFF; verify in hex before flashing (dump stock's 0x01010018 region and match — exact stock bytes UNVERIFIED here) |
| R6 | `arduino-renesas-bootloader` has **no LICENSE file** | For internal use: low practical risk; for redistribution: contact Arduino or reimplement DFU portion (TinyUSB itself is MIT) |
| R7 | DFU-uploaded sketches carry no CRC (stock behavior) | Bootloader computes and writes the metadata block itself after DFU download (§5.2), restoring "never jump unvalidated" |
| R8 | Metadata block vs >238 KB sketches; EEPROM-library conflict if metadata moved to data flash | Last-code-flash-block default; document the 238 KB app limit |
| R9 | Flash erase/write timings, VBTBKR register count, PFS PSEL code for CAN0, CAN register bit-level details — all UNVERIFIED | RA4M1 HW Manual (R01UM0007) chapters: Option-Setting Memory, Flash Memory, CAN Module, I/O Ports; verify during bring-up |
| R10 | Transceiver logic level (5 V board) | TJA1050/MCP2551/MCP2562; not SN65HVD230 |

Suggested next steps: (1) clone + build stock bootloader unmodified, flash via rfp-cli, confirm byte-identical behavior; (2) write `memory_map.h` + linker script + host-tested `image_validate.c`; (3) polled CAN driver spike as an *application* sketch first (easier debugging with Arduino_CAN as cross-check); (4) integrate into fork behind the VBTBKR[4] magic; (5) fault-injection acceptance tests per `rules/testing.md` (corrupt a byte → refuses boot; kill power mid-update → recovers in bootloader).

---

## 10. Sources

Primary (source code / authoritative):

- Stock bootloader repo: <https://github.com/arduino/arduino-renesas-bootloader> — `src/main.c` (double-tap, boot5, flash), `src/flash.h` (SKETCH_FLASH_OFFSET = 16 KB, PROGRAM_BLOCK_SIZE), `Makefile.minima` (PID 0x0369, DFU descriptor `8*2Ka,120*2Kg`)
- Bootloader build/flash README: <https://github.com/arduino/ArduinoCore-renesas/blob/main/bootloaders/UNO_R4/README.md>
- Shipped hex (size + OFS bytes measured): <https://raw.githubusercontent.com/arduino/ArduinoCore-renesas/main/bootloaders/UNO_R4/dfu_minima.hex>
- Arduino core (MIT): <https://github.com/arduino/ArduinoCore-renesas> — `variants/MINIMA/memory_regions.ld` (0x4000, RAM/data-flash map), `variants/MINIMA/fsp.ld` (OFS/ID-code sections, `.sketch_boot`, "ROM registers at 0x400"), `variants/MINIMA/pins_arduino.h` + `pinmux.inc` + `variant.cpp` (D4=P103=CAN TX, D5=P102=CAN RX), `variants/MINIMA/includes/ra_gen/bsp_clock_cfg.h` (HOCO 48 MHz, PCLKB 24 MHz, FCLK 24 MHz), `cores/arduino/usb/SerialUSB.cpp` + `cores/arduino/boot.h` (DFU-detach handler, magic 0x07738135, VBTBKR[0]), `libraries/Arduino_CAN/src/R7FA4M1_CAN.cpp` (PCLKB clock source, bit-timing limits), `boards.txt` (`use_1200bps_touch=false`)
- FSP source: <https://github.com/renesas/fsp> — `ra/fsp/src/bsp/mcu/ra4m1/bsp_feature.h` (CF write 8 B / block 2 KB; DF write 1 B / block 1 KB; 1 CAN channel)
- FSP r_flash_lp docs: <https://renesas.github.io/fsp/group___f_l_a_s_h___l_p.html> (no CF access during P/E, FCLK ≥ 4 MHz, disable interrupts)
- FSP r_can docs: <https://renesas.github.io/fsp/group___c_a_n.html> (32 mailboxes, FIFO 24–31, PCLKB/CANMCLK, PCLKA:PCLKB 2:1 on RA4M1, interrupt-driven API)
- Renesas KB — running flash driver from RAM: <https://en-support.renesas.com/knowledgeBase/20037102>

Datasheets / manuals:

- RA4M1 Group Datasheet: <https://www.renesas.com/en/document/dst/ra4m1-group-datasheet>
- RA4M1 Hardware User's Manual (mirror): <https://cdn.sparkfun.com/assets/b/1/d/3/6/RA4M1_Datasheet.pdf>
- Renesas RA standard boot firmware app note (ROM boot mode): <https://www.renesas.com/en/document/apn/renesas-ra-family-system-specifications-standard-boot-firmware>
- Renesas flash memory programming app note: <https://www.renesas.com/en/document/apn/flash-memory-programming>

Board / ecosystem:

- Arduino UNO R4 Minima CAN tutorial (transceiver required, D4/D5): <https://docs.arduino.cc/tutorials/uno-r4-minima/can>
- UNO R4 Minima user manual: <https://docs.arduino.cc/tutorials/uno-r4-minima/cheat-sheet>
- Zephyr UNO R4 docs (pyOCD target `r7fa4m1ab`, unbrick command): <https://docs.zephyrproject.org/latest/boards/arduino/uno_r4/doc/index.html>
- UNO R4 Minima pinout reference (BOOT pin explanation): <https://lastminuteengineers.com/arduino-uno-r4-minima-pinout/>
- Bootloading forum guide (BOOT–GND + rfp-cli): <https://forum.arduino.cc/t/how-to-bootload-the-arduino-r4-wifi-minima-with-the-renesas-processor/1328579>
- UNOR4CAN register-level CAN library (reference for polled driver): <https://github.com/obdevel/UNOR4CAN>

Protocols / host tooling:

- OpenBLT (XCP on CAN, GPLv3): <https://www.feaser.com/openblt/doku.php?id=faq>
- OpenBLT overview: <https://osrtos.com/library/openblt/>
- python-can: <https://python-can.readthedocs.io>
