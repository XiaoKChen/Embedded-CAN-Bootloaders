# UNO R4 Minima CAN+DFU Bootloader — Feature & Behavior Specification

**Purpose:** implementation-independent requirements document for a full rewrite (Rust/Embassy).
Captures WHAT the C bootloader on the default branch does and every hardware-derived constant —
not how the C code is organized.

**Source of truth:** the `main` branch of this repo (the task brief said `master`; the repo's
default branch is `main` — there is no `master`). Every constant below was read from source via
`git show main:<path>`; citations are `path` (as on `main`). Nothing is guessed; the handful of
facts the repo itself marks unverified are collected in §9.4.

**Target:** Arduino UNO R4 Minima, Renesas **R7FA4M1AB3CFM#AA0** (RA4M1, Cortex-M4F, 48 MHz,
64-pin LQFP, 5 V logic) — `docs/research/r4-minima-can-bootloader.md` §3.

---

## 1. Memory map & partitions

Single source of truth on `main`: `src/boot/memory_map.h` (mirrors the vendored linker layout,
`docs/reference/minima_fsp.ld` + the Arduino `memory_regions.ld` values); compile-time
`_Static_assert`s enforce partition consistency.

### 1.1 Code flash (256 KB, 128 × 2 KB erase blocks)

| Region | Address range | Size | Blocks | Contents |
|---|---|---|---|---|
| Bootloader | `0x0000_0000`–`0x0000_3FFF` | 16 KB (`BOOT_SIZE 0x4000`) | 8 | Bootloader code + vector table at 0x0 + **OFS0/OFS1 option bytes at 0x400/0x404** (Security-MPU words through 0x43F). **Never self-written at runtime.** |
| Application slot | `0x0000_4000`–`0x0003_F7FF` | 238 KB (`APP_SLOT_SIZE 0x0003_B800` = 243,712 B) | 119 | App vector table at `APP_BASE 0x4000` (stock-compatible; Arduino IDE unchanged) |
| Image metadata block | `0x0003_F800`–`0x0003_FFFF` | 2 KB (last block, `METADATA_ADDR 0x0003F800`) | 1 | 16-byte image metadata record (§3), written **last** during an update |

- Flash erase block: **2048 B** (`FLASH_BLOCK_SIZE`); code-flash **write unit: 8 B**
  (`FLASH_WRITE_UNIT`, FSP `r_flash_lp` MF3 macro) — writes are 8-byte-aligned multiples.
- Code flash is memory-mapped at address **0** (the C build needs
  `-fno-delete-null-pointer-checks` because `flash_base = (const uint8_t *)0`;
  `vendor/arduino-renesas-bootloader/Makefile.minima`).
- RA4M1 has **no dual-bank flash**: code flash cannot be read while being programmed/erased; the
  flash routines execute from RAM (FSP handles placement) with **interrupts disabled** during P/E.
  FCLK ≥ 4 MHz required (actual FCLK = 24 MHz). (research doc §3.3)

### 1.2 Data flash (8 KB at `0x4010_0000`, 1 KB erase blocks, 1 B write unit)

| Region | Address | Size | Contents |
|---|---|---|---|
| Data flash | `0x4010_0000`–`0x4010_1FFF` | 8 KB (`DATA_FLASH_SIZE`) | Mostly app/EEPROM-library territory |
| DFU config block | `0x4010_1C00` (`DFU_CONFIG_ADDR`, **last 1 KB block**) | ≤ 80 B used | Flash-time DFU name + per-device CAN-ID config (§6.2). **Frozen shared contract** with the external ESP-Prog-2 provisioning flasher — do not move. |

Data flash is readable via normal loads **only** after `DFLCTL.DFLEN = 1` plus a **tDSTOP ≈ 6 µs**
settle, with the FACI in read mode — see the fail-safe in §2.4.

### 1.3 SRAM

| | |
|---|---|
| SRAM | `0x2000_0000`–`0x2000_7FFF`, 32 KB (`SRAM_BASE`/`SRAM_SIZE`) |
| `SRAM_END` | `0x2000_8000` — exclusive; legal as a full-descending initial-SP top |

### 1.4 Backup registers (handoff words, survive reset)

`R_SYSTEM->VBTBKR[]` is a **byte array** in the VBATT backup domain. Writes require PRCR PRC1
unlock: `R_SYSTEM->PRCR = 0xA502`, write, re-lock `0xA500` (`vendor/.../src/flash.h`,
`src/main.c`).

| Word | Bytes | Magic | Meaning |
|---|---|---|---|
| `VBTBKR[0..3]` | 4 | `0x07738135` (`DOUBLE_TAP_MAGIC`) | Stock double-tap / DFU-detach request → USB DFU mode |
| `VBTBKR[4..7]` | 4 | `0xB007CAFE` (`CAN_UPDATE_MAGIC`, `src/boot/boot_config.h`) | App-requested CAN update mode |
| `VBTBKR[8..11]` | 4 | `0x5D1A6435` | CAN_DIAG staircase run-once flag (diagnostic builds only) |

### 1.5 Option bytes & ID code (bootloader owns them — brick risk)

- **OFS0 = `0xFFFFFFFF`** (IWDT/WDT auto-start disabled), **OFS1 = `0xFFFFCEDF`**, at flash
  `0x400`/`0x404`; Security-MPU defaults follow through `0x43F`. Must be byte-identical to stock
  (`docs/build.md` "Pre-flash check", expected dump given there). Wrong OFS0 can auto-start the
  IWDT against a bootloader that never feeds it.
- **ID code (OSIS)** at `0x0101_0018` (0x20 bytes): keep all `0xFF` = debugger/boot-mode
  protection disabled, or SWD/ROM-boot recovery is locked out. (research doc §3.4, risk R5)

### 1.6 Linker constraints

- Vector table at 0; `.sketch_boot` alignment then fixed vectors, because "ROM registers are at
  address 0x400" (`docs/reference/minima_fsp.ld`): the vector area is padded to keep 0x400 free
  for OFS.
- App linker (`memory_regions.ld` in the Arduino core) sets `FLASH_IMAGE_START = 0x4000`; the
  bootloader's `SKETCH_FLASH_OFFSET = 16 * 1024` must equal `APP_BASE` (static-asserted in
  `src/main.c`).
- Build-time **size gate: the bootloader `.bin` must be ≤ 16,384 B** (`Makefile.minima`
  `BOOT_REGION_LIMIT`). Current C build: 15,576 B (gcc 14.2.1) — 808 B headroom.

---

## 2. Boot decision logic

From `vendor/arduino-renesas-bootloader/src/main.c` `main()` (CAN_LOADER build), in order:

1. **Double-tap / DFU magic:** if `VBTBKR[0..3] == 0x07738135` → clear it (PRC1-unlocked write)
   → goto bootloader (combined **DFU + CAN** service mode, §2.5).
2. **CAN update magic:** if `VBTBKR[4..7] == 0xB007CAFE` → clear it →
   `run_can_update_mode()` (**CAN-only** mode, §2.3). Never returns; exits only via
   `NVIC_SystemReset()`.
3. **Double-tap arming window:** if the reset was *not* power-on (`!R_SYSTEM->RSTSR0_b.PORF`):
   write `DOUBLE_TAP_MAGIC` to `VBTBKR[0]`, busy-wait until `board_millis() >= 500` (500 ms),
   then clear it. A second reset tap inside that window re-enters step 1.
4. **App validation** (§3.3): `image_validate(flash_base=0)`:
   - `IMAGE_VALID` → app valid.
   - `IMAGE_ERR_MAGIC` or `IMAGE_ERR_HEADER_CRC` (metadata absent/erased/corrupt) → **fallback
     to the stock check**: app valid iff `(*(uint32_t*)0x4000 & 0xFF000000) == 0x20000000`
     (initial SP word points into SRAM). Documented deviation: covers images flashed by tools
     that write no metadata (e.g. SWD).
   - Any other failure (size, SP, entry, payload CRC) → app invalid — **fail closed**.
5. App valid → `boot5(0x4000)` (§2.2). Else fall through to the bootloader service mode (§2.5).

### 2.1 Entry paths, summarized

| Trigger | Written by | Result |
|---|---|---|
| `VBTBKR[0] = 0x07738135` + reset | Arduino core DFU-detach handler / double-tap reset | DFU+CAN parked mode |
| `VBTBKR[4] = 0xB007CAFE` + `NVIC_SystemReset()` | Application (e.g. Airform `/enter_bootloader` RPC handler) | CAN-only update mode |
| No valid app in slot | — | DFU+CAN parked mode |
| Valid app, no magic | — | Jump to app |

There is **no boot-loop counter** and **no CAN listen window at boot** (the research doc §5.2
mentions both as options; neither is implemented on `main`).

### 2.2 App entry sequence (`boot5`, stock behavior kept)

```
if CAN was initialized: can_polled_deinit()          // module-stop CAN0, pins → reset state
PRCR PRC1-unlock; module-stop USBFS and USBHS; re-lock
R_MPU_SPMON->SP[0].CTL = 0                           // disable MSP monitoring
__disable_irq(); __DSB(); __ISB()
SysTick->CTRL = 0
SCB->VTOR = 0x4000                                   // BOOTLOADER sets VTOR, app does not
MSP = *(uint32_t*)0x4000
jump to *(uint32_t*)0x4004                            // app reset handler, Thumb
```

The jump happens **with IRQs disabled**; the application re-enables them. (VTOR policy is
documented in `src/boot/memory_map.h`.)

### 2.3 CAN-only update mode (`run_can_update_mode`)

Order matters (each step is a hard-won fix):

1. **Power the CAN transceiver rail** (`can_xcvr_usb_rail_power_on`, §5.3) **before any CAN
   register access**.
2. **Data-flash read gate** (`flash_lp_open_df_read_ready`, §2.4) → latch `dfu_config_readable`.
3. Load per-device CAN IDs from the DFU config block (defaults if unproven/absent/invalid/v1).
4. Init the protocol engine with the effective cmd ID; `can_polled_init(250000, cmd_id)`.
   If CAN init fails → `NVIC_SystemReset()` (back into the normal boot decision).
5. Loop forever: `can_service_once()` (§4.6). Exits only via the GO reset or external reset.

### 2.4 Data-flash read fail-safe (commit `2100f5e` — CPU-lockup fix)

**Hardware quirk:** entry via the app's `NVIC_SystemReset()` (unlike a pin reset) preserves
data-flash/FACI state. Loads from the data-flash window are only legal when `DFLCTL.DFLEN = 1`,
tDSTOP (~6 µs) has elapsed after its 0→1 edge, and the FACI is in read mode. Violating this
returns garbage at best and **bus-faults at worst** — and the image's BusFault/HardFault handlers
are FSP's `Default_Handler` → `__BKPT(0)`, which with no debugger attached **locks the core**
(dead board until pin reset / power cycle).

`R_FLASH_LP_Open()` normally enables DFLEN and waits tDSTOP, **but** (a) it skips the settle when
DFLCTL already reads 1 (state that can survive a software reset), and (b) it can fail before
touching DFLEN (FCLK gate) or after entering data-flash P/E mode (P/E-exit timeout on a FACI left
dirty by the app), whereupon `R_FLASH_LP_Reset()` is a silent no-op (not-open param check).

**Required behavior:** the only proof of data-flash readability is `R_FLASH_LP_Open()` **and**
`R_FLASH_LP_Reset()` both returning success, followed by an **unconditional re-assert of
`DFLCTL.DFLEN = 1` plus a 6 µs settle** (`DATA_FLASH_TDSTOP_SETTLE_US`). Latch that proof;
**nothing may read `DFU_CONFIG_ADDR` while it is false** — compiled default IDs and an empty
INFO name are used instead. Fail-safe: the board always parks and serves CAN, never a dead board.
This gate is also used on the DFU-path `R_FLASH_LP_Open` in `main()`.

### 2.5 Parked / recovery mode (DFU + CAN simultaneously)

`run_bootloader()`: `tud_init()` + `board_init_after_tusb()` (USB DFU up), load per-device CAN
IDs, init CAN engine + driver, then loop
`{ tud_task(); led_blinking_task(); if (can_initialized) can_service_once(); }`.
Both transports stay alive; a CAN init failure just disables the CAN half. LED: GPT channel 3
PWM fade on P111 (`LED_FADE_GPIO=BSP_IO_PORT_01_PIN_11`), 100 ms step, `FADESTEP 2000`,
period `0x9896` counts (Makefile.minima flags).

---

## 3. Image format & validation

### 3.1 Metadata record (`src/boot/image_header.h`)

Exactly **16 bytes, little-endian**, at `METADATA_ADDR 0x0003_F800`:

| Offset | Size | Field | Value |
|---|---|---|---|
| 0x0 | u32 | `magic` | `0xB007D00D` (`IMAGE_META_MAGIC`, `src/boot/boot_config.h`) |
| 0x4 | u32 | `image_size` | bytes covered by `image_crc32`, from `APP_BASE` |
| 0x8 | u32 | `image_crc32` | CRC32 over flash `[APP_BASE, APP_BASE + image_size)` |
| 0xC | u32 | `header_crc32` | CRC32 over the **first 12 bytes** of this record as stored |

Written **last** during an update (the commit point). Power loss at any earlier instant leaves
the block erased → image unvalidated → device parks in the bootloader — never bricked.

### 3.2 CRC32 (`src/boot/crc32.c/h`)

IEEE 802.3 / zlib-compatible: **reflected, polynomial `0xEDB88320`, init `0xFFFFFFFF`, final XOR
`0xFFFFFFFF`**. Golden vector: `crc32("123456789") == 0xCBF43926`. C impl is a table-less bitwise
loop (deliberate flash-size tradeoff; the Rust rewrite may choose any implementation with
identical output).

### 3.3 Validation (`src/boot/image_validate.c`), check order — fail closed, first failure wins

1. `magic == 0xB007D00D` else `IMAGE_ERR_MAGIC`
2. `crc32(meta[0..12]) == header_crc32` else `IMAGE_ERR_HEADER_CRC`
3. `8 <= image_size <= APP_SLOT_SIZE` else `IMAGE_ERR_SIZE` (min 8 = SP + entry words)
4. Initial SP (word at `APP_BASE`): `SRAM_BASE < sp <= SRAM_END` else `IMAGE_ERR_SP`
   (`0x2000_8000` legal = full-descending stack top; `0x2000_0000` illegal = zero-size stack)
5. Reset vector (word at `APP_BASE+4`): Thumb bit (bit 0) **set**, and
   `APP_BASE <= (entry & ~1) < APP_BASE + APP_SLOT_SIZE`, else `IMAGE_ERR_ENTRY`
6. `crc32(app[0..image_size]) == image_crc32` else `IMAGE_ERR_PAYLOAD_CRC` (expensive check last)

All multi-byte reads are explicit little-endian byte loads (no aliased/misaligned loads). Pure
logic over a byte buffer — host-testable (`tests/test_image_validate.c`).

---

## 4. CAN Update Protocol v1 (frozen wire contract)

Definitions: `src/boot/can_update.h/.c` (device engine) and `tools/can-flash/can_flash.py`
(host); both agree on every constant below.

### 4.1 Bus parameters

| Parameter | Value |
|---|---|
| CAN flavor | Classic CAN 2.0B, **11-bit standard IDs only**, no RTR/extended/FD |
| Bit rate | **250,000 bit/s** (`CAN_BITRATE_BPS`; chosen for HOCO clock-tolerance margin) |
| Frame size | Every protocol frame **DLC 8**; unused payload bytes filled `0xFF` |
| Host→boot ID (default) | `0x79E` (`CAN_ID_HOST_TO_BOOT`) — engine match + driver RX filter |
| Boot→host ID (default) | `0x79F` (`CAN_ID_BOOT_TO_HOST`) — all replies/INFO frames |
| Per-device override | v2 DFU config block (§6.2) may replace either ID (each independently), valid range `0x000..0x7FF` |

### 4.2 Commands (host → boot; byte0 = command)

Required order **HELLO → SIZE → ERASE → DATA… → VERIFY → GO**. Protocol version `0x01`.

| Cmd | Byte0 | Payload | Accepted in phase |
|---|---|---|---|
| HELLO | `0x01` | byte1 = protocol version (`0x01`) | **ANY** phase — restarts the session (documented deviation for host-retry robustness) |
| ERASE | `0x02` | — | SIZED only |
| DATA | `0x03` | byte1 = seq (u8, starts 0, mod 256 across whole transfer); bytes2..7 = 6 image bytes (final frame may carry fewer valid bytes — receiver counts against image_size) | TRANSFER only |
| SIZE | `0x04` | bytes2..5 = image_size, u32 LE | GREETED only |
| VERIFY | `0x05` | bytes2..5 = expected image CRC32, u32 LE | TRANSFERRED only |
| GO | `0x06` | — | VERIFIED only |
| INFO | `0x07` | — | Any time; answered by the integration layer, not the engine (§4.7); does **not** touch session state |

Phases: `IDLE → GREETED → SIZED → TRANSFER → TRANSFERRED → VERIFIED` (`can_update_phase_t`).

### 4.3 Replies (boot → host, one frame, DLC 8)

```
ACK: {0x79, cmd_echo, 0x00, arg, 0xFF, 0xFF, 0xFF, 0xFF}
NAK: {0x79, cmd_echo, err,  arg, 0xFF, 0xFF, 0xFF, 0xFF}
```

- Byte0 marker `0x79` (`CAN_UPDATE_REPLY_MARKER`); byte2 = `0x00` (OK) or error code.
- **HELLO ACK special form:** byte3 = bootloader protocol version (`0x01`), bytes4..7 =
  `APP_SLOT_SIZE` u32 LE (= `0x0003B800`, 243,712).
- **DATA window ACK:** arg (byte3) = last accepted seq.
- **DATA seq NAK:** err `0x01`, arg = first seq of the current window (rewind target).

Error codes:

| Code | Name | Meaning |
|---|---|---|
| `0x01` | `ERR_SEQ` | unexpected DATA seq; arg = seq to rewind to; window buffer discarded |
| `0x02` | `ERR_FLASH` | program/erase/read-back failure |
| `0x03` | `ERR_CRC` | VERIFY CRC mismatch (metadata NOT written — fail closed) |
| `0x04` | `ERR_SIZE` | SIZE zero or > APP_SLOT_SIZE |
| `0x05` | `ERR_STATE` | out-of-order command, unknown command, bad HELLO version, or DLC ≠ 8 |

Frame handling rules (engine, `can_update_handle_frame`):
- Frames with a different CAN ID: silently ignored (no reply).
- DLC ≠ 8 on the engine's ID: NAK `ERR_STATE` echoing byte0.
- Unknown command / command in wrong phase: NAK `ERR_STATE`.

### 4.4 Windowing & flow control

- One **window = one 2048-byte flash block** = **342 DATA frames** (341 × 6 B + 1 × 2 B), or the
  final short block. Frames never straddle a block (host packs per-window); device clamps
  against both block boundary and image_size regardless.
- **Mid-window DATA frames get NO reply.** The block-completing frame is ACKed only after
  `flash_program` (write + read-back verify) succeeds. Rationale: during a code-flash write the
  CPU is stalled with IRQs off and the RX FIFO is only 4 deep — the host must stop-and-wait per
  window.
- Tail padding: the final partial block is padded with `0xFF` (erased value) to a whole number
  of 8-byte write units before programming.
- On unexpected seq: NAK `ERR_SEQ` with arg = the window's first seq; the device discards the
  window buffer (completed blocks stay counted) and expects retransmission from that seq. The
  device NAKs **every** subsequent out-of-order frame of the stale burst.
- On flash failure mid-transfer: NAK `ERR_FLASH`, window discarded, device stays in TRANSFER
  (host resends the window).
- Seq wrap hazard (host-side mitigation): seq is u8 and a 342-frame window wraps mod 256;
  consecutive window-start seqs differ by 342 mod 256 = 86 (period 128 windows > 119 max blocks,
  so a rewind seq maps to exactly one window). Host polls for early NAKs every 32 frames so a
  NAK is noticed before ~256 more frames are queued.

### 4.5 ERASE semantics (fail-safe ordering)

`flash_erase_app_and_metadata`: erase the **metadata block FIRST** (invalidates the old image
before any app byte is disturbed — fail closed), then all 119 app-slot blocks, one block per
IRQ-off window (keeps USB serviceable in the combined loop). 120 blocks total; takes seconds
(host waits up to 15 s for the ERASE ACK).

Power-loss table (no state bricks; the bootloader region is never written):

| Power lost… | Result |
|---|---|
| after ERASE | metadata erased → image invalid → stays in bootloader |
| between DATA blocks | metadata still erased → stays in bootloader |
| during VERIFY's metadata write | metadata torn → header CRC fails → stays in bootloader |
| after VERIFY | new image fully validated → boots |

### 4.6 VERIFY / GO

- VERIFY: device recomputes CRC32 over flash `[APP_BASE, APP_BASE+image_size)` and compares to
  the host's value. On match, it builds the 16-byte metadata record (header CRC over the first
  12 bytes) and programs it at `METADATA_ADDR` (16 B = 2 write units) — **the commit point** —
  then ACKs. Mismatch → NAK `ERR_CRC`, metadata stays erased.
- GO: ACK, then the integration layer deinits CAN (`can_polled_deinit`) and calls
  `NVIC_SystemReset()` — clean re-entry through the boot decision (which then validates and
  boots the new image). GO is not a direct jump.
- Service loop per received frame (`can_service_once`): receive → if cmd==INFO on our ID answer
  INFO → else feed engine → transmit reply if produced (loss tolerated; host retries) → perform
  reset if GO accepted.

### 4.7 INFO (CMD `0x07`) — board identification, 5-frame reply

Answered by the integration layer (not the single-reply engine). Host sends
`{0x07, FF×7}` on the device's cmd ID; device replies on its reply ID, exactly 5 DLC-8 frames:

| Frame | Content |
|---|---|
| 0 (header) | `{0x79, 0x07, name_len, protocol_version(0x01), 0xFF, 0xFF, 0xFF, 0xFF}` |
| 1..4 | the **32-byte product-name field**, 8 bytes/frame in order, ASCII, **0x00-padded** beyond `name_len` |

Name source: `dfu_config_parse` of the config block at `DFU_CONFIG_ADDR` — read only when the
data-flash gate proved readability. Unproven/absent/invalid block (or a build without the
config feature) → `name_len = 0`, all 32 bytes `0x00`.

### 4.8 Host-side session (mirror view, `tools/can-flash/can_flash.py`)

- Timeouts: ordinary command ACK **2.0 s**; ERASE **15 s**; default **3 retries** per step;
  idempotent commands are re-sent on timeout; any NAK on a non-DATA command is fatal.
- HELLO checks the reply's protocol version equals `0x01` and reads the slot size (expects
  `0x0003B800`; only warns if different). Rejects images larger than the announced slot.
- DATA: sends a full window then waits for the window ACK; checks ACK arg == expected last seq
  (stale ACKs from resends are ignored and the window retried). A lost window ACK is handled by
  resending the window — the bootloader then NAKs `ERR_SEQ` pointing at the *next* window's
  start seq, which the host interprets as "advance" (rewind map). `ERR_FLASH` → resend window.
  After any NAK the host drains the reply queue until 150 ms of RX silence (NAK bursts).
- VERIFY uses `zlib.crc32(image)`.

---

## 5. CAN hardware bring-up on RA4M1 (`src/drivers/can_polled.c/h`)

### 5.1 Why a polled driver

FSP's `r_can` is interrupt-driven by design (callback API, no polled read). The bootloader is
poll-only (no interrupts, no static-init-order deps, no heap) — so a small register-level driver
against `R_CAN0` is used. Register facts were cross-checked against FSP sources
(`vendor/ra4m1-headers/reference/r_can.c/h`) and the RA4M1 HW manual (R01UM0007) CAN chapter.

### 5.2 Configuration used

| Item | Value | Source/rationale |
|---|---|---|
| Peripheral | **CAN0** (RA4M1's only CAN channel; classic 2.0B, no FD; max 1 Mbit/s; 32 mailboxes) | research §4.2 |
| Pins | **D4 = P103 = CTX0 (TX)**, **D5 = P102 = CRX0 (RX)** — ArduinoCore-renesas MINIMA pinmux | `can_polled.c` |
| Pin routing | PFS PSEL code for CAN = **0x10** (bits [28:24]); PMR bit 16 = peripheral mode. PWPR unlock: write 0 (clear B0WI), write PFSWE (bit 6); write PSEL with PMR=0 then set PMR (two writes); re-lock: write 0, write B0WI (bit 7) | `can_polled.c` |
| Module stop | CAN0 = MSTPCRB **bit 2**; release before register access, dummy read-back to complete the write; set on deinit | `can_polled.c` |
| CAN clock | **PCLKB = 24 MHz** (`BCR.CCLKS = 0`). Board clocks: no crystal; HOCO 48 MHz → ICLK 48, PCLKA 48, PCLKB 24, FCLK 24. FSP constraint: PCLKA:PCLKB must be 2:1 for CAN on this part | `can_polled.h`, research §3.5 |
| Bit timing | **16 tq/bit: SS=1, TSEG1=11, TSEG2=4, SJW=1 → sample point 75 %**. BRP (prescaler): **6** for 250 k, **3** for 500 k (BCR fields hold value−1). Only 250 k and 500 k accepted; anything else is an init error | `can_polled.c` |
| Mailbox scheme | **FIFO mailbox mode** (`CTLR.MBM=1`), standard-ID (`IDFM=00`), overwrite (`MLM=0`). RX FIFO = mailboxes **28–31 (4-deep)**; TX from **mailbox 0**. Mailboxes 0–23 disabled/cleared. `MKIVLR = 0x00FFFFFF` (bits 24–31 must stay 0 in FIFO mode) | `can_polled.c` |
| Acceptance filter | **Exact single-ID**: both `FIDCR[0]`/`FIDCR[1]` = rx_filter_id, both `MKR[6]`/`MKR[7]` = `0x7FF` (compare every SID bit). SID sits at bits [28:18] of MB.ID/MKR/FIDCR | `can_polled.c` |
| Mode sequencing | CTLR.CANM: 00=operate, 01=reset, 10=halt; poll `STR & 0x0700` until it mirrors `CANM<<8`. BCR/MBM/IDFM/MLM writable only in **reset** mode (entering reset also clears latched bus-off + error counters); mailbox/mask/FIDCR config in **halt** mode; enable RX FIFO (`RFCR.RFE=1`) only after entering **operate** mode | `can_polled.c` |
| Interrupts | None: `MIER = 0`, `EIER = 0` | `can_polled.c` |
| RX (non-blocking) | Bus-off check (`STR.BOST`) → FIFO-empty check (`RFCR.RFEST`) → read FIFO head via **MB[28]** window → write `0xFF` to `RFPCR` to advance the CPU-side pointer → if `RFCR.RFMLF` (message lost), rewrite `RFCR = RFE` to clear it. DLC>8 on the wire is clamped to 8 | `can_polled.c` |
| TX (bounded poll) | Write MB[0] ID (IDE=0, RTR=0)/DL/data, set `MCTL_TX[0] = TRMREQ (0x80)`, poll `SENTDATA (0x01)` up to **2,000,000 loops** (~200 ms at 48 MHz); on success or timeout clear MCTL by **writing 0x00 twice** (SENTDATA and TRMREQ cannot clear in one write — hardware quirk); timeout aborts the request | `can_polled.c` |
| Mode-switch bound | 100,000 poll loops | `can_polled.c` |
| Bus-off recovery | On `STR.BOST` in send or receive: full driver re-init (same bitrate/filter), report BUS_OFF status; caller keeps waiting/retrying — never proceed with a half-programmed slot | `can_polled.c` |
| Deinit | Enter reset mode (result ignored), set module-stop bit, restore P102/P103 PmnPFS to `0x00000000` (GPIO input). Called before jump-to-app and before the GO reset | `can_polled.c` |

### 5.3 USB-regulator / transceiver-rail power-up (commits `e2d941d`, `8e3e65f`)

**Hardware quirk (bench-confirmed via a one-boot self-bisecting "staircase" diagnostic on a real
bus):** on custom boards the external CAN transceiver / level-shifter path is powered from the
RA4M1's **on-chip USB regulator rail (VCC_USB)**. Until that regulator is on, CRX0 never shows
recessive: the CAN controller reaches operate mode, TRMREQ sets, but **TEC stays frozen at 0** —
the controller never sees bus-idle. Deaf and mute. The USB-DFU path always initialized USB
(which powers the rail), which is why double-tap entry always worked and only the
magic-handoff (CAN-only) path was silent.

**Required behavior on CAN-update-mode entry, BEFORE any CAN register access** (power only — no
USB stack):

1. Release USBFS module-stop (`R_BSP_MODULE_START(FSP_IP_USBFS)` — MSTPCRB bit 11).
2. `SYSCFG.SCKE = 1` (USB clock supply — gates the other USBFS registers), bounded readback wait
   (1000 loops); a timeout falls through deliberately (worst case the board parks degraded, never
   dead).
3. `USBMC.VDCEN = 1` (USB LDO regulator ON).
4. Soak **5 ms** before the first CAN access (no VDCEN ramp figure exists in FSP/HW manual;
   5 ms is comfortably above typical LDO start-up + rail charge).

Harmless on boards without the dependency: the LDO merely energizes an unused VCC_USB pad.

### 5.4 External hardware assumptions

The Minima has **no on-board transceiver**; CAN0 is logic-level only. A **5 V** transceiver
(TJA1050 / MCP2551 / MCP2562 / TCAN332) is required: D4→TXD, D5→RXD, 5V→VCC, common GND; 120 Ω
termination at both bus ends. 3.3 V-only parts (SN65HVD230) must not be used on this 5 V board.

---

## 6. USB DFU path (retained from upstream, plus additions)

### 6.1 Stock behavior kept

The upstream Arduino `arduino-renesas-bootloader` (commit `581a59d0…`, tinyusb 0.17.0 + one
patch) USB DFU path is retained unchanged in behavior, memory map, and identity:

- USB VID/PID: **0x2341 / 0x0369**; product string default "UNO R4 Minima DFU", manufacturer
  "Arduino"; serial = chip-ID hex string (`usb_descriptors.c`, `Makefile.minima`).
- USB DFU 1.1 via TinyUSB `tud_dfu`; DFU flash descriptor
  **`@CodeFlash /0x00000000/8*2Ka,120*2Kg`** (8×2 KB read-only bootloader + 120×2 KB
  erase/write app region) plus a `@DataFlash /0x08000000/8*1Kg` alt; attributes: upload +
  download + manifestation-tolerant, detach timeout 1000 ms, transfer size
  `CFG_TUD_DFU_XFER_BUFSIZE`.
- Download: per 2 KB `PROGRAM_BLOCK_SIZE`, erase-on-boundary then `R_FLASH_LP_Write`, IRQs
  disabled around each flash op. Upload supported (memcpy from flash). DFU_DETACH → drain USB
  100 ms → `NVIC_SystemReset()`.
- `BOSSA_LOADER` (UART second transport, used by other boards) is **not** enabled for the Minima
  build; only `DFU_LOADER` is. The Arduino IDE upload workflow (DFU runtime detach from the
  running sketch, not 1200-bps touch) keeps working.

### 6.2 Flash-time DFU name + per-device CAN-ID config block (`src/boot/dfu_config.h/.c`)

Read at boot from data flash `DFU_CONFIG_ADDR 0x4010_1C00`. **FROZEN CONTRACT** shared with the
external ESP-Prog-2 provisioning flasher. Two on-disk versions; the bootloader accepts both:

| Offset | Size | Field | v1 (76 B) | v2 (80 B) |
|---|---|---|---|---|
| 0x00 | u32 LE | magic | `0xDF0C0FDF` | same |
| 0x04 | u8 | version | `1` | `2` |
| 0x05 | u8 | mfr_len | 0..32 | same |
| 0x06 | u8 | prod_len | 0..32 | same |
| 0x07 | u8 | reserved | `0xFF` | same |
| 0x08 | char[32] | manufacturer (ASCII, `0xFF` past mfr_len) | ✓ | ✓ |
| 0x28 | char[32] | product (ASCII, `0xFF` past prod_len) | ✓ | ✓ |
| 0x48 | u32 LE | **v1**: crc32 over bytes 0x00..0x47 (72 B) | ✓ | — |
| 0x48 | u16 LE | **v2**: `boot_cmd_id` (host→boot, 11-bit) | — | ✓ |
| 0x4A | u16 LE | **v2**: `boot_reply_id` (boot→host, 11-bit) | — | ✓ |
| 0x4C | u32 LE | **v2**: crc32 over bytes 0x00..0x4B (76 B) | — | ✓ |

Validity (ALL must hold, else full fallback): magic matches AND version ∈ {1,2} AND both lens
≤ 32 AND the version's CRC matches. Result codes ordered to the check order:
`OK / ERR_MAGIC / ERR_VERSION / ERR_LEN / ERR_CRC`.

- **Names:** valid block → manufacturer (USB string index 1) and product (index 2) come from the
  block (UTF-16-widened at enumeration); else compiled defaults. Independent of the CAN feature.
- **CAN IDs:** only a valid **v2** block supplies them; each ID is *independently* validated as
  ≤ `0x7FF` (0xFFFF / out-of-range = "unset" → that ID's compiled default). Absent/invalid/v1
  block → both defaults. The effective cmd ID becomes the driver's RX acceptance filter and the
  engine's match ID; every reply/INFO frame goes out on the effective reply ID. Purpose: several
  boards parked in the bootloader can share one bus, each individually addressable.
- CRC of the block = same CRC32 as §3.2.

### 6.3 DFU-download metadata write-back (research risk R7)

Stock DFU uploads carry no CRC. After a completed DFU download (`tud_dfu_manifest_cb`), the
bootloader computes CRC32 over the received image (`dfu_received_len` accumulated across
download callbacks) and writes a fresh metadata block — so DFU-uploaded sketches are
CRC-validated on later boots too. Guards: skip if `dfu_received_len == 0` or
`> APP_SLOT_SIZE` (an oversized sketch already overwrote the metadata block; such images are
simply unvalidated — the SP-check fallback still boots them). Every early-exit path degrades to
the stock SP-check fallback, never a brick.

---

## 7. Host tool (`tools/can-flash/can_flash.py`)

Single-file Python tool, PEP 723 inline deps (`uv run` capable): **Python ≥ 3.9,
python-can ≥ 4.0, can-isotp ≥ 2.0, textual ≥ 0.60**. Any python-can adapter (SocketCAN, slcan/
CANable, gs_usb/candleLight, PCAN, Kvaser…). Runs as an interactive Textual TUI on bare
invocation (or `--tui`), headless CLI otherwise. Exit 0 on success, 1 on failure, 130 on Ctrl-C.

CLI (defaults): `firmware.bin` positional; `--interface` / `--channel` (TUI defaults
`socketcan`/`can0`); `--bitrate 250000`; `--id-tx 0x79E`; `--id-rx 0x79F`; `--retries 3`;
`--no-go` (flash+verify, skip GO); `--info` (INFO query + exit); `--scan` (+ `--scan-from 0x700`
`--scan-to 0x71F`); `--enter-bootloader` (+ `--board-rx 0x07` `--board-tx 0x08`); `--selftest`;
`--tui`.

Behaviors beyond the protocol session (§4.8):

- **`--scan` discovery:** INFO-probes each candidate cmd ID one at a time (band `0x700..0x71F`
  **plus** legacy `0x79E` **plus** known-device IDs `{0x01,0x03,0x05,0x07,0x09}`, always
  included), listening **broadly on any reply ID** (a board's reply ID is arbitrary);
  0.3 s per probe; prints a NAME/CMD_ID/REPLY_ID/PROTO table.
- **`--enter-bootloader`:** reboots a *running* app into the CAN bootloader via the Airform
  `/enter_bootloader` RPC — a `Uart_Packet` `{0x55, addr_size u16 BE, addr, body_size u16 BE,
  body, crc}` (CRC = XOR of header + body_size bytes + body bytes; address excluded), 23 bytes
  for the empty-body route, sent as **ISO-TP 15765-2** (11-bit normal addressing,
  txid=board-rx 0x07, rxid=board-tx 0x08, TX padding 0x00). Fire-and-forget (the handler writes
  `0xB007CAFE` to VBTBKR[4] and resets without replying); tool waits 2 s for the reboot.
- **TX backpressure:** treats ENOBUFS/"buffer full" as transient and retries up to 2 s (a
  342-frame window burst outruns socketcan queues).
- **`--selftest`:** full flow against an in-process fake bootloader on python-can's `virtual`
  bus, incl. a forced frame drop exercising the rewind path, a 3-board scan test, the
  `/enter_bootloader` golden vector, and a headless TUI smoke test.

Bench-verified end-to-end (commit `e2d941d`): `--enter-bootloader` → INFO on provisioned IDs →
full 80 KB flash in 7.6 s → GO → app returns. A sibling pure-bash Debian package
(`airform-tui-can-flasher`, ARM/Linux) implements the same flow.

---

## 8. Build, CI & flashing (context for the rewrite)

- **Chip:** R7FA4M1AB (pyOCD target `r7fa4m1ab`; J-Link device `R7FA4M1AB`).
- **Toolchain (C build):** xPack GNU Arm Embedded gcc **14.2.1**, `-Os -flto
  -ffunction-sections -fdata-sections -Wl,--gc-sections`, CPU flags `-mcpu=cortex-m4
  -mfpu=fpv4-sp-d16 -mfloat-abi=hard -mthumb`, C11. Build via tinyusb's Make system
  (`Makefile.minima`, `BOARD=uno_r4`), toggles `CAN_LOADER=1`, `DFU_CUSTOM_NAME=1`,
  `CAN_DIAG=0`. Size gate at 16,384 B fails the build if exceeded.
- **CI (`.github/workflows/release.yml` + `scripts/ci-build.sh`):** every push/PR to `main`
  builds on ubuntu with the pinned toolchain (cached), fetches the gitignored FSP
  (`d52e5a6a…`) and CMSIS_5 (`20285262…`) deps, enforces the size gate, uploads
  `.hex/.bin/.elf` artifacts; pushing a `v*` tag publishes a GitHub Release with version-named
  artifacts. No embedded firmware version field — the release tag is the version. **Worth
  carrying forward:** pinned toolchain, size gate as a hard build failure, tag-driven releases.
- **Flashing the bootloader itself** (stock DFU cannot — region is `2Ka` read-only):
  1. ROM boot mode: BOOT pad→GND, replug → "Renesas RA USB Boot" CDC →
     `rfp-cli -device ra -port COMx -p <hex>`.
  2. SWD: `pyocd flash -e sector -a 0x0 -t r7fa4m1ab <hex>`.
  - Unbrick: same paths with the stock `docs/reference/dfu_minima.hex`. Mask-ROM boot cannot be
    erased → chip-level unbrickable while the ID code stays `0xFF`.
- **Host tests (C):** Unity/CTest over the pure-logic core — `tests/test_crc32.c`,
  `test_image_validate.c`, `test_can_update.c`, `test_dfu_config.c` (33+ tests). The pure
  logic (CRC, validation, protocol engine, config parse) has zero hardware dependencies by
  design — preserve that seam in Rust (host-testable `logic/` crate).

---

## 9. Known quirks & lessons learned (mined from `git log main` + docs)

### 9.1 Hardware gotchas (each cost a debugging campaign — do not rediscover)

1. **Deaf transceiver rail (`e2d941d`):** custom boards power the CAN transceiver from the USB
   LDO rail. CAN-only entry must set MSTPCRB USBFS release + `SYSCFG.SCKE` + `USBMC.VDCEN` +
   5 ms soak **before any CAN access** (§5.3). Symptom without it: operate mode reached, TRMREQ
   set, TEC frozen at 0, board silent on every ID. Found via a run-once self-bisecting boot
   staircase (VBTBKR[8] flag) that replayed the DFU preamble piece by piece with a CAN TX probe
   after each — step "+tud_init" was the winner.
2. **Software-reset FACI state (`2100f5e`):** `NVIC_SystemReset()` preserves data-flash/FACI
   state (a pin reset does not). Reading data flash without the proven Open+Reset+re-assert-
   DFLEN+6 µs gate can bus-fault into `__BKPT(0)` = locked core = dead board (§2.4). Never read
   `DFU_CONFIG_ADDR` (or any data-flash address) unguarded; fall back to defaults, keep serving
   CAN.
3. **MCTL double-clear:** CAN0 `SENTDATA` and `TRMREQ` cannot be cleared in one register write —
   write `0x00` twice (FSP `r_can.c` does the same).
4. **RX FIFO is 4 deep + CPU stalls during flash P/E with IRQs off** → the protocol's
   ACK-per-2 KB-window pacing is load-bearing, not stylistic. Mid-window frames must not expect
   replies; the host must never stream past an outstanding window.
5. **HOCO-clocked CAN** (no crystal): bit timing derives from the ±~1 % internal oscillator.
   250 kbit/s at 75 % sample point chosen for tolerance margin; 500 k supported but not default.
   PCLKA:PCLKB must be 2:1 for CAN on this part (board default already satisfies it).
6. **OFS0/OFS1 at 0x400/0x404 live inside the bootloader image** — wrong values auto-start the
   IWDT or worse; ID code must stay `0xFF` or SWD/ROM-boot lock out (§1.5).
7. **Code flash mapped at address 0**: any language/toolchain that treats address-0 reads as UB
   needs the equivalent of `-fno-delete-null-pointer-checks` (in Rust: use raw pointer/volatile
   reads, never references to address 0).
8. **First-TX-after-bring-up loss** is tolerated by design: reply loss is acceptable at every
   point because the host retries idempotent commands and HELLO resets the session from any
   phase.
9. **Erase-metadata-first ordering** is the entire power-loss story (§4.5): old image is
   invalidated before any app byte changes; new metadata written only after CRC verify.
10. **Bus-off** during an update: re-init the controller and keep waiting; never proceed with a
    half-programmed slot, never reset out of the update mode on bus errors.

### 9.2 Protocol/host lessons

- HELLO accepted in any phase = host-retry robustness (deliberate deviation from strict
  ordering; every other command NAKs `ERR_STATE` out of order).
- Seq-u8 wrap: 342-frame windows wrap mod 256 — host must notice a NAK within ~256 frames
  (early-poll every 32 frames) and window-start seqs are unique per transfer only because
  119 ≤ 128 (period of 86 mod 256). Keep the window/seq geometry if wire compatibility matters.
- A lost window ACK is disambiguated by resending the window: the device NAK's rewind target
  then tells the host whether the window was actually accepted.
- socketcan TX-queue backpressure (ENOBUFS / "Transmit buffer full") is normal when bursting
  342-frame windows; retry with a deadline.

### 9.3 Diagnostic tooling worth reproducing

The `CAN_DIAG=1` build (compiled out by default, byte-identical off) provided: stage-marker
frames `{0xD1, stage, …}` through the magic-entry path, a retry-based bring-up bisect, and the
run-once staircase (§9.1.1) with register-snapshot payloads (STR/TECR/ECSR tell you: mode
actually reached? controller ever drove the bus? how frames died — ACKEF vs stuff/bit errors).
Diag builds deliberately omit the rail power-up fix so they keep reproducing the failure, and
park on the bench instead of resetting. A Rust rewrite should plan an equivalent defmt/CAN-frame
diagnostic story for bring-up on new board revisions.

### 9.4 Facts the repo itself marks unverified (research doc §9)

- Exact HOCO tolerance spec; flash erase/program times per block (order-of-ms assumed);
  RA4M1 VBTBKR total byte count; exact stock ID-code byte values (assumed 0xFF); OFS field
  bit meanings beyond the stock values; VDCEN regulator ramp time (5 ms soak is an engineering
  margin, not a datasheet figure).

---

## 10. Requirements checklist for the Rust rewrite

Memory & layout
- [ ] R1. Flash partition exactly: bootloader `0x0–0x3FFF` (16 KB), app slot `0x4000–0x3F7FF`
      (243,712 B, 119×2 KB), metadata block `0x3F800–0x3FFFF` (2 KB); compile-time-checked.
- [ ] R2. Bootloader image ≤ 16,384 B, enforced as a hard build failure.
- [ ] R3. Bootloader never writes flash below `0x4000` at runtime.
- [ ] R4. OFS0 `0xFFFFFFFF` / OFS1 `0xFFFFCEDF` emitted at 0x400/0x404; Security-MPU words
      0x408–0x43F byte-identical to stock; ID code region all `0xFF`.
- [ ] R5. App vector table at `0x4000`; bootloader sets `VTOR = 0x4000` before the jump (app
      need not).

Boot decision
- [ ] R6. `VBTBKR[0..3] == 0x07738135` → clear → DFU+CAN parked mode.
- [ ] R7. `VBTBKR[4..7] == 0xB007CAFE` → clear → CAN-only update mode.
- [ ] R8. All VBTBKR writes PRC1-unlocked (`PRCR = 0xA502` … `0xA500`).
- [ ] R9. Non-POR reset arms a 500 ms double-tap window (write magic, wait, clear).
- [ ] R10. App validation: metadata magic `0xB007D00D` → header CRC → size ∈ [8, 243712] →
      SP ∈ (0x2000_0000, 0x2000_8000] → entry Thumb-bit set and within the app slot →
      payload CRC; fail-closed, first failure wins, in this order.
- [ ] R11. Metadata absent/corrupt (magic or header-CRC failure only) falls back to the stock
      SP-word check `(*(u32*)0x4000 & 0xFF000000) == 0x20000000`; all other validation
      failures park in the bootloader.
- [ ] R12. Jump sequence: deinit CAN (if up), module-stop USBFS/USBHS, disable SP monitor,
      IRQs off + DSB/ISB, SysTick off, VTOR, MSP from `0x4000`, jump `*(0x4004)`; IRQs left
      disabled for the app.
- [ ] R13. GO performs `NVIC_SystemReset()` after deinit — never a direct jump from update mode.

CRC & metadata
- [ ] R14. CRC32: reflected poly `0xEDB88320`, init/xorout `0xFFFFFFFF`;
      `crc32("123456789") == 0xCBF43926`.
- [ ] R15. Metadata record: 16 B LE `{magic 0xB007D00D, image_size, image_crc32,
      header_crc32(first 12 B)}`, written only after a successful VERIFY (or DFU write-back).

CAN protocol (wire-compatible with v1)
- [ ] R16. Classic CAN 2.0B, 11-bit IDs, 250 kbit/s, every protocol frame DLC 8, fill `0xFF`.
- [ ] R17. Default IDs `0x79E` (cmd) / `0x79F` (reply); per-device override from a valid v2
      config block, each ID independently validated ≤ `0x7FF` with per-ID fallback.
- [ ] R18. Commands `0x01..0x07` and phase machine exactly as §4.2; HELLO restarts the session
      from any phase; everything else NAKs `ERR_STATE 0x05` out of order; DLC≠8 NAKs
      `ERR_STATE`; foreign IDs silently ignored.
- [ ] R19. Replies `{0x79, cmd_echo, err, arg, FF×4}`; HELLO ACK carries version + slot size
      u32 LE; DATA ACK arg = last accepted seq; seq NAK arg = window-start seq.
- [ ] R20. SIZE bounds: reject 0 and > 243,712 with `ERR_SIZE 0x04`.
- [ ] R21. ERASE: metadata block first, then all 119 app blocks; interrupt-safe chunking so the
      companion transport stays alive; ACK may take seconds (host budget 15 s).
- [ ] R22. DATA windowing: 6 bytes/frame, one 2048-B block per window (342 frames, 341×6+1×2),
      frames never straddle blocks, mid-window frames unacknowledged, block ACKed only after
      program + read-back verify, tail 0xFF-padded to 8-B write units.
- [ ] R23. Seq errors: NAK `ERR_SEQ 0x01` with rewind seq, discard window buffer, NAK every
      further stale frame; flash errors: NAK `ERR_FLASH 0x02`, discard window, stay in
      TRANSFER.
- [ ] R24. VERIFY: recompute CRC over flash, mismatch → NAK `ERR_CRC 0x03` with metadata left
      erased; match → write metadata (commit point) → ACK.
- [ ] R25. INFO `0x07`: 5-frame reply (header + 32-byte 0x00-padded ASCII product name), served
      outside the session state machine, name from the config block only when data flash is
      proven readable, else `name_len=0` + zero bytes.
- [ ] R26. Power loss at any instant yields either a fully-validated image or a parked
      bootloader (table §4.5) — provable from the write ordering.

CAN driver
- [ ] R27. CAN0, pins P103/CTX0 (D4) + P102/CRX0 (D5), PFS PSEL `0x10`, PWPR-locked writes,
      PSEL-before-PMR ordering.
- [ ] R28. PCLKB 24 MHz source; timing 16 tq (SS1/TSEG1 11/TSEG2 4/SJW 1, 75 % SP); BRP 6 @
      250 k (3 @ 500 k); reject other bitrates.
- [ ] R29. FIFO mailbox mode; RX FIFO MB28–31 with exact-match single-ID filtering
      (FIDCR0/1 + MKR6/7 = 0x7FF, MKIVLR high byte 0); TX mailbox 0 with bounded SENTDATA
      poll (~200 ms) and the double `0x00` MCTL clear; poll-only (MIER/EIER = 0).
- [ ] R30. Mode transitions confirmed by polling `STR & 0x0700`; BCR/MBM written in reset mode,
      mailbox config in halt mode, RFCR.RFE set after operate.
- [ ] R31. Bus-off (STR.BOST): re-init and keep serving; RFMLF cleared by rewriting RFE;
      DLC > 8 clamped.
- [ ] R32. Deinit restores module-stop and both pins' PFS to reset (GPIO input) before any
      jump/reset.

Fail-safes & rails
- [ ] R33. CAN-only entry powers the USB rail first: USBFS module-start, SCKE with bounded
      wait (fall through on timeout), VDCEN, 5 ms soak — before any CAN register access.
- [ ] R34. Data-flash reads gated on proven readability (flash Open + Reset success + explicit
      DFLEN re-assert + ≥6 µs settle); unproven → compiled defaults, empty INFO name, keep
      serving CAN. Applies to every data-flash read on every entry path.
- [ ] R35. CAN init failure in CAN-only mode resets into the normal boot decision; in parked
      mode it degrades to DFU-only. No failure path may dead-loop or BKPT.

USB DFU (if the rewrite keeps it — the C bootloader does)
- [ ] R36. USB DFU 1.1, VID 0x2341 / PID 0x0369, descriptor `@CodeFlash /0x00000000/
      8*2Ka,120*2Kg`, stock Arduino IDE detach-upload flow preserved.
- [ ] R37. DFU manufacturer/product strings overridden from a valid config block (v1 or v2) at
      enumeration; compiled defaults otherwise.
- [ ] R38. After a completed DFU download of ≤ slot-size bytes, compute CRC32 and write a fresh
      metadata block; skip (leaving stock fallback validation) for zero-length or oversized
      downloads and on any flash error.
- [ ] R39. Parked mode services USB DFU and the CAN engine in one loop; CAN flash operations
      must not starve USB beyond one erase/program block per IRQ-off window.

Config block
- [ ] R40. Parse v1 (76 B) and v2 (80 B) blocks at `0x4010_1C00` per the §6.2 layout; validity =
      magic `0xDF0C0FDF` + version ∈ {1,2} + lens ≤ 32 + version-specific CRC32; fail closed
      with the compiled defaults; layout is a frozen external contract — no changes.

Quality gates
- [ ] R41. Protocol engine, image validation, CRC, and config parsing are pure host-testable
      logic (no HAL/PAC/executor deps) with tests covering: golden CRC vector, every
      validation failure code, every NAK path, window rewind, seq wrap, HELLO-restart,
      v1/v2/invalid config, per-ID fallback, and the power-loss ordering invariants.
- [ ] R42. Host flasher compatibility: an unmodified `tools/can-flash/can_flash.py` (defaults:
      250 k, 0x79E/0x79F, 3 retries, 2 s/15 s timeouts) must flash the rewritten bootloader
      end-to-end, including `--info`, `--scan`, and `--enter-bootloader`.
- [ ] R43. CI builds with a pinned toolchain, enforces the size gate, and publishes
      version-named artifacts on `embassy-ra4m1-v*` tags.
