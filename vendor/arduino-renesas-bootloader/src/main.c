/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2019 Ha Thach (tinyusb.org)
 * Copyright (c) 2023 Arduino SA
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "bsp/board_api.h"
#include "tusb.h"
#include "bsp_api.h"
#ifdef LED_FADE_PWM_CHANNEL
#include "r_gpt.h"
#endif

#include "flash.h"
#include "can_diag.h"  /* no-op hooks unless CAN_LOADER && CAN_DIAG */

#ifdef CAN_LOADER
/* Flash-over-CAN update path (UNO R4 Minima): pure-logic engine + validation
 * from the repo's /src tree, polled CAN0 driver on D4/D5. See
 * docs/research/r4-minima-can-bootloader.md and src/boot/memory_map.h. */
#include "boot/can_update.h"
#include "boot/crc32.h"
#include "boot/dfu_config.h"  /* DFU_CONFIG_STR_MAX; dfu_config_parse (INFO)   */
#include "boot/image_validate.h"
#include "drivers/can_polled.h"

static bool can_initialized = false;
#endif

_Alignas(8) uint8_t aucbuffer[PROGRAM_BLOCK_SIZE];  /* flash program source: one 8 B write unit */
uint16_t auclen = 0;
uint16_t auc_block_num = 0;

void bgo_callback(flash_callback_args_t *p_args)
{
  if (p_args->event == FLASH_EVENT_WRITE_COMPLETE && auclen > 0) {
    tud_dfu_finish_flashing(DFU_STATUS_OK);
    auclen = 0;
  }
}

void flash_ready_interrupt_handler();
void flash_error_interrupt_handler();

flash_instance_ctrl_t g_flash_ctrl;
const flash_cfg_t g_flash_cfg =
{ .data_flash_bgo = false, .p_callback = bgo_callback, .p_context = NULL,
  .irq = 7,
  .err_irq = 8,
  .err_ipl = (2),
  .ipl = (2),
};

/* Instance structure to use this module. No .p_api: every caller uses the
 * R_FLASH_* functions directly, and the API table would keep the whole driver
 * (ID code, access window, startup-area swap, ...) linked into the 16 KB
 * region. */
const flash_instance_t g_flash ={ .p_ctrl = &g_flash_ctrl, .p_cfg = &g_flash_cfg };

//--------------------------------------------------------------------+
// MACRO CONSTANT TYPEDEF PROTYPES
//--------------------------------------------------------------------+

/* Blink pattern
 * - 250 ms  : device not mounted
 * - 1000 ms : device mounted
 * - 2500 ms : device is suspended
 */
enum  {
  BLINK_NOT_MOUNTED = 250,
  BLINK_MOUNTED = 1000,
  BLINK_SUSPENDED = 2500,
};

static uint32_t blink_interval_ms = BLINK_NOT_MOUNTED;

/* Set by tud_dfu_detach_cb (inside tud_task); the main loop drains USB for
 * DFU_DETACH_DRAIN_MS and resets, so tud_task is never re-entered. */
static bool dfu_detach_requested;
#define DFU_DETACH_DRAIN_MS 100u

void led_blinking_task(void);
void set_double_tap_data(uint32_t data);

/* Jump sequence per rules/bootloader.md: IRQs off first, then deinit every
 * peripheral the bootloader touched, stop SysTick and clear its pending bit,
 * disable and clear every NVIC interrupt, then VTOR, MSP and the reset
 * vector. The application re-enables IRQs itself. */
void boot5(uint32_t address) {
  __disable_irq();

#ifdef CAN_LOADER
  /* No current caller reaches here with CAN up; kept self-contained. */
  if (can_initialized) {
    can_polled_deinit();
    can_initialized = false;
  }
#endif

  R_SYSTEM->PRCR = (uint16_t) BSP_PRV_PRCR_PRC1_UNLOCK;
  R_BSP_MODULE_STOP(FSP_IP_USBFS, 0);
  R_BSP_MODULE_STOP(FSP_IP_USBHS, 0);
  R_SYSTEM->PRCR = (uint16_t) BSP_PRV_PRCR_LOCK;

    /* Disable MSP monitoring. */
#if BSP_FEATURE_TZ_HAS_TRUSTZONE
    __set_MSPLIM(0);
#else
    R_MPU_SPMON->SP[0].CTL = 0;
#endif

  SysTick->CTRL = 0;
  SysTick->LOAD = 0;
  SysTick->VAL  = 0;
  SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk;
  for (uint32_t i = 0; i < sizeof(NVIC->ICER) / sizeof(NVIC->ICER[0]); i++) {
    NVIC->ICER[i] = 0xFFFFFFFFu;
    NVIC->ICPR[i] = 0xFFFFFFFFu;
  }
  __DSB();
  __ISB();

  SCB->VTOR  = address;

  uint32_t mainStackPointer = *(volatile uint32_t *)(address);
  __set_MSP(mainStackPointer);
  uint32_t programResetHandlerAddress = *(volatile uint32_t *) (address + 4);
  void (* programResetHandler)(void) = (void (*)(void)) programResetHandlerAddress;
  programResetHandler();
}

#if defined(RENESAS_CORTEX_M23)
#define BOOT_DOUBLE_TAP_DATA              (*((volatile uint32_t *)0x20007FF0))
#else
#define BOOT_DOUBLE_TAP_DATA              (*((volatile uint32_t *) &R_SYSTEM->VBTBKR[0]))
#endif
#define DOUBLE_TAP_MAGIC                  0x07738135

#ifdef CAN_LOADER
/* CAN-update request handoff: the word at VBTBKR[4..7] (VBTBKR is a byte
 * array; VBTBKR[0..3] stays the stock DFU double-tap word). Written by the
 * application before reset, cleared here on entry; PRC1 unlock required,
 * exactly as for BOOT_DOUBLE_TAP_DATA. */
/* VBTBKR is a byte array; bytes 4..7 form a naturally aligned word (the
 * uintptr_t round-trip keeps -Wcast-align quiet about it). */
#define CAN_UPDATE_REQUEST_DATA           (*((volatile uint32_t *) (uintptr_t) &R_SYSTEM->VBTBKR[4]))

_Static_assert(SKETCH_FLASH_OFFSET == APP_BASE,
               "memory_map.h APP_BASE must match the stock SKETCH_FLASH_OFFSET");

static void set_can_update_request(uint32_t data)
{
  R_SYSTEM->PRCR = (uint16_t) BSP_PRV_PRCR_PRC1_UNLOCK;
  CAN_UPDATE_REQUEST_DATA = data;
  R_SYSTEM->PRCR = (uint16_t) BSP_PRV_PRCR_LOCK;
}

/* ---- Data-flash read gate ----------------------------------------------
 * Target-specific (RA4M1 flash, FSP r_flash_lp MF3): loads from the data
 * flash window (0x4010_0000, DFU_CONFIG_ADDR) are legal only when
 * DFLCTL.DFLEN = 1 and tDSTOP (~6 us; FSP FLASH_LP_WAIT_TDSTOP) has elapsed
 * after the 0->1 edge, and the FACI is in read mode. Violating that returns
 * garbage at best and bus-faults at worst — and this image's BusFault/
 * HardFault handlers are FSP's Default_Handler -> __BKPT(0), which locks the
 * core up with no debugger attached: a dead board until the next reset pin /
 * power cycle.
 *
 * R_FLASH_LP_Open() normally enables DFLEN and waits tDSTOP, BUT
 *   (a) it SKIPS the settle when DFLCTL already reads 1 (state that can
 *       survive the application's NVIC_SystemReset() on the magic-handoff
 *       entry — unlike the pin reset of the double-tap entry), and
 *   (b) it can fail before touching DFLEN at all (FCLK gate) or after
 *       entering data-flash P/E mode (P/E-exit timeout on a FACI left dirty
 *       by the application), and R_FLASH_LP_Reset() is then a silent no-op
 *       (FSP param check: not-open).
 * So the only PROOF of readability is Open and Reset both returning
 * FSP_SUCCESS, plus an unconditional re-assert + settle that does not depend
 * on FSP's skip-if-already-1 shortcut. dfu_config_readable latches that
 * proof; nothing reads DFU_CONFIG_ADDR while it is false — the compiled
 * default IDs / empty INFO name are used instead (fail-safe: the board
 * always keeps serving CAN, never a dead board). */
#define DATA_FLASH_TDSTOP_SETTLE_US  6u

bool dfu_config_readable = false;  /* also read by usb_descriptors.c */

static BOOT_LOW_FLASH bool flash_lp_open_df_read_ready(void)
{
#ifdef CAN_DIAG
  /* Same calls/short-circuits as stock, with the fsp_err_t values latched
   * for the stage-1 marker frame (can_diag.c). */
  can_diag_open_err = (uint16_t) R_FLASH_LP_Open(g_flash.p_ctrl, g_flash.p_cfg);
  if (can_diag_open_err != (uint16_t) FSP_SUCCESS) {
    return false;
  }
  can_diag_reset_err = (uint16_t) R_FLASH_LP_Reset(g_flash.p_ctrl);
  if (can_diag_reset_err != (uint16_t) FSP_SUCCESS) {
    return false;
  }
#else
  if (R_FLASH_LP_Open(g_flash.p_ctrl, g_flash.p_cfg) != FSP_SUCCESS) {
    /* Driver down: every flash HAL op fails cleanly (FSP_ERR_NOT_OPEN). */
    return false;
  }
  if (R_FLASH_LP_Reset(g_flash.p_ctrl) != FSP_SUCCESS) {
    return false;
  }
#endif
  /* Open guarantees DFLEN = 1 on success; re-assert and settle anyway so the
   * read gate holds by local proof, not by FSP internals. Safe here: the
   * successful Reset above left the FACI in read mode. */
  R_FACI_LP->DFLCTL = 1U;
  R_BSP_SoftwareDelay(DATA_FLASH_TDSTOP_SETTLE_US, BSP_DELAY_UNITS_MICROSECONDS);
  return true;
}

/* ---- can_update engine flash HAL (r_flash_lp) --------------------------
 * Power-loss outcomes of every mutation here are enumerated in
 * src/boot/can_update.h; in short: the update-in-progress marker is in
 * place before the app slot is erased (update_start_plan decides what that
 * costs), and the record is written last, so an interruption at any instant
 * leaves either the old app intact or the marker parking the device in this
 * bootloader (boot_decide). The bootloader's own 8 flash blocks
 * (0x0000-0x3FFF) are never touched. Shared with the USB DFU download. */

/* One block per IRQ-off window, so USB stays serviceable between blocks. */
static bool flash_erase_block(uint32_t addr)
{
  __disable_irq();
  fsp_err_t err = R_FLASH_LP_Erase(g_flash.p_ctrl, addr, 1);
  __enable_irq();
  return err == FSP_SUCCESS;
}

static bool can_flash_program(uint32_t addr, const uint8_t *data, uint32_t len)
{
  __disable_irq();
  int err = R_FLASH_LP_Write(g_flash.p_ctrl, (uint32_t) data, addr, len);
  __enable_irq();
  if (err != FSP_SUCCESS) {
    return false;
  }
  /* Read-back verify: a failed verify is a failed write, never "probably
   * fine" (rules/bootloader.md). */
  return memcmp((const void *) addr, data, len) == 0;
}

/* Leave the update-in-progress marker programmed and the record area blank.
 * Callers must not erase any app block unless this returns true: until then
 * a power loss still boots the old app, or parks. update_start_plan keeps
 * the marker of an interrupted update (erasing it would hand the partial
 * image to the SP-word fallback), and erases the APP_BASE block first when
 * the slot is not bootable, for the same reason. */
static bool flash_mark_update_pending(void)
{
  update_start_t plan = update_start_plan((const uint8_t *) FLASH_BASE);
  if (plan == UPDATE_START_MARKER_IN_PLACE) {
    return true;
  }
  if (plan == UPDATE_START_ERASE_APP_THEN_METADATA && !flash_erase_block(APP_BASE)) {
    return false;
  }
  if (!flash_erase_block(METADATA_ADDR)) {
    return false;
  }
  /* Non-const so the source stays in RAM, like every other write here: code
   * flash is not readable while it is in P/E mode. */
  _Alignas(FLASH_WRITE_UNIT) uint32_t marker[2] = { IMAGE_PENDING_MAGIC,
                                                    (uint32_t) ~IMAGE_PENDING_MAGIC };
  return can_flash_program(METADATA_ADDR + IMAGE_PENDING_OFFSET,
                           (const uint8_t *) marker, sizeof(marker));
}

static bool can_flash_erase_app_and_metadata(void)
{
  if (!flash_mark_update_pending()) {
    return false;
  }
  for (uint32_t block = 0; block < (APP_SLOT_SIZE / FLASH_BLOCK_SIZE); block++) {
    if (!flash_erase_block(APP_BASE + block * FLASH_BLOCK_SIZE)) {
      return false;
    }
  }
  return true;
}

/* Flash owner latch between the two update transports of the parked loop:
 * the USB DFU download owns flash from block 0 until manifest, abort or
 * failure (dfu_owns_flash); the CAN session owns it from SIZE until it
 * returns to IDLE/GREETED (can_update_owns_flash). Each side refuses to
 * start while the other owns it, so their erases and writes never
 * interleave. A reset releases both. */
static bool dfu_owns_flash;

static bool can_flash_locked(void)
{
  return dfu_owns_flash;
}

static const can_update_hal_t can_hal = {
  .flash_erase_app_and_metadata = can_flash_erase_app_and_metadata,
  .flash_program                = can_flash_program,
  .flash_base                   = (const uint8_t *) 0,  /* code flash is mapped at 0 */
  .flash_locked                 = can_flash_locked,
};

static can_update_state_t can_state;

/* Effective per-device CAN update IDs. Default to the compiled constants; a
 * DFU_CUSTOM_NAME build reads the v2 config block (see can_load_ids) so several
 * parked boards can share one bus, each addressed on its own ID. The engine
 * matches RX frames on can_cmd_id (also the driver's RX filter) and every reply
 * / INFO frame goes out on can_reply_id. */
static uint32_t can_cmd_id   = CAN_ID_HOST_TO_BOOT;
static uint32_t can_reply_id = CAN_ID_BOOT_TO_HOST;

/* Load the per-device CAN IDs from the DFU config block at DFU_CONFIG_ADDR.
 * Reads only when dfu_config_readable proved the data-flash window readable
 * (see flash_lp_open_df_read_ready); an unproven window, an absent/invalid/v1
 * block or an out-of-range ID field leaves the compiled defaults
 * (dfu_config_can_ids fills them in). A build without DFU_CUSTOM_NAME has no
 * config parser, so it keeps the defaults. */
static void can_load_ids(void)
{
#ifdef DFU_CUSTOM_NAME
  if (!dfu_config_readable) {
    return;  /* data flash unproven: keep the compiled default IDs */
  }
  uint16_t cmd_id;
  uint16_t reply_id;
  dfu_config_can_ids((const uint8_t *) DFU_CONFIG_ADDR, &cmd_id, &reply_id);
  can_cmd_id   = cmd_id;
  can_reply_id = reply_id;
#endif
}

/* Replies are best effort (the host retries). A failed bus-off recovery marks
 * CAN down: CAN update mode then resets, the parked loop re-inits it. */
static void can_send_reply(const uint8_t frame[8])
{
  if (can_polled_send(can_reply_id, 8, frame) == CAN_POLLED_RECOVERY_FAILED) {
    can_initialized = false;
  }
}

/* Every exit from a CAN service path re-runs the boot decision. */
static void can_deinit_and_reset(void)
{
  can_polled_deinit();  /* deinit every peripheral this path touched */
  NVIC_SystemReset();
}

/* INFO (host CMD 0x07): report the device's USB product name so a host can
 * identify a board parked in the bootloader. Answered here, not in the update
 * engine, because the reply is 5 frames (the engine emits one per frame).
 *
 * Reply on can_reply_id (frozen contract):
 *   frame 0  {0x79, 0x07, name_len, protocol_version, 0xFF, 0xFF, 0xFF, 0xFF}
 *   frames 1..4  the 32-byte product-name field, 8 bytes each in order,
 *                ASCII, 0x00-padded beyond name_len.
 *
 * Name source: dfu_config_parse of the v1 (76 B) or v2 (80 B) block at
 * DFU_CONFIG_ADDR (data
 * flash, read only when dfu_config_readable proved the window readable — see
 * flash_lp_open_df_read_ready). Unproven window or absent/invalid block ->
 * name_len = 0 and all 32 name bytes 0x00. A build without DFU_CUSTOM_NAME
 * has no config parser, so it reports name_len = 0. */
static void can_service_info(void)
{
  uint8_t name[DFU_CONFIG_STR_MAX];
  uint8_t name_len = 0;

  memset(name, 0x00, sizeof(name));  /* contract: 0x00 padding beyond name_len */

#ifdef DFU_CUSTOM_NAME
  const char *mfr;
  const char *prod;
  uint8_t mfr_len;
  uint8_t prod_len;
  if (dfu_config_readable &&
      dfu_config_parse((const uint8_t *) DFU_CONFIG_ADDR,
                       &mfr, &mfr_len, &prod, &prod_len) == DFU_CFG_OK) {
    name_len = prod_len;              /* dfu_config_parse guarantees <= 32 */
    memcpy(name, prod, prod_len);
  }
#endif

  uint8_t frame[8];
  frame[0] = CAN_UPDATE_REPLY_MARKER;      /* 0x79 */
  frame[1] = CAN_UPDATE_CMD_INFO;          /* 0x07 */
  frame[2] = name_len;                     /* 0..32 */
  frame[3] = CAN_UPDATE_PROTOCOL_VERSION;  /* same version HELLO reports (1) */
  memset(&frame[4], CAN_UPDATE_FILL_BYTE, 4);
  can_send_reply(frame);

  for (uint32_t off = 0; off < DFU_CONFIG_STR_MAX; off += 8u) {
    can_send_reply(&name[off]);
  }
}

/* Poll one frame through the update engine, transmit its reply, and perform
 * the GO reset. Shared by the CAN-only mode and the combined DFU+CAN loop.
 * Returns true once an accepted update-protocol frame has been handled
 * (can_update_accepts_frame: can_cmd_id, DLC 8, command byte HELLO..INFO,
 * 0xFF padding); the CAN-only mode's idle timeout restarts only on those.
 * Provisioned boards share can_cmd_id with their app's request ID, so the
 * master's periodic app polls (ISO-TP) land here too: they get no reply and
 * must not keep a stranded board in CAN-only mode. */
static bool can_service_once(void)
{
  /* Zero-init: out-params are written by the callees before use, but gcc 7
   * LTO cannot prove it and -Werror=maybe-uninitialized trips otherwise. */
  uint32_t id = 0;
  uint8_t dlc = 0;
  uint8_t data[8] = {0};
  uint8_t reply[8] = {0};
  can_update_action_t action = CAN_UPDATE_ACTION_NONE;

  can_polled_status_t status = can_polled_receive(&id, &dlc, data);
  if (status == CAN_POLLED_RECOVERY_FAILED) {
    can_initialized = false;
  }
  if (status != CAN_POLLED_OK) {
    return false;  /* empty FIFO, bus-off (the controller recovers), or CAN down */
  }

  if (!can_update_accepts_frame(&can_state, id, dlc, data)) {
    return false;
  }

  /* INFO is intercepted before the update engine: its 5-frame reply does not
   * fit the engine's one-reply-per-frame contract. Answering it does not touch
   * or advance the update session state. */
  if (data[0] == CAN_UPDATE_CMD_INFO) {
    can_service_info();
    return true;
  }

  if (can_update_handle_frame(&can_state, id, dlc, data, reply, &action)) {
    can_send_reply(reply);
  }
  if (action == CAN_UPDATE_ACTION_RESET) {
    can_deinit_and_reset();
  }
  return true;
}

#ifndef CAN_DIAG
/* ---- CAN transceiver rail power-up (production fix) ----------------------
 * Hardware-confirmed via the DIAG4 staircase (one boot, real bus): CAN0 TX
 * is dead on the magic-entry path — operate mode reached, TRMREQ set, TEC
 * frozen at 0, i.e. the engine never sees bus-idle — at every preamble step
 * UNTIL tud_init() + board_init_after_tusb() run, after which the first send
 * succeeds. Board-level dependency: the external CAN transceiver/level-
 * shifter path is powered from the RA4M1's on-chip USB regulator rail
 * (VCC_USB), so CRX0 never shows recessive until that regulator is on. The
 * DFU path always powers it; this CAN-only path never did — the original
 * "silent CAN update mode" bug.
 *
 * Replicate ONLY the power-relevant writes of the proven DFU bring-up, in
 * its order, without starting the USB stack:
 *   1. USBFS module-stop release, MSTPCRB b11 (rusb2_ra.h:84-91)
 *   2. SYSCFG.SCKE = 1 — USB clock supply, gates the other USBFS registers
 *      (dcd_rusb2.c:696-697)
 *   3. USBMC.VDCEN = 1 — USB LDO regulator ON (board_init_after_tusb,
 *      hw/bsp/ra/family.c:138; RA4M1 USBMC b7 "Regulator ON/OFF control")
 * then let the regulator soak before the first CAN access. Harmless on
 * boards without the dependency: the LDO merely energizes an unused VCC_USB
 * pad. Compiled OUT of CAN_DIAG builds — the staircase (can_diag.c) must
 * keep reproducing the unpowered-rail conditions it diagnosed. */
#define CAN_USB_RAIL_SCKE_TIMEOUT_LOOPS  1000u
#define CAN_USB_RAIL_SETTLE_MS           5u

static void can_xcvr_usb_rail_power_on(void)
{
  R_BSP_MODULE_START(FSP_IP_USBFS, 0);

  R_USB_FS0->SYSCFG |= R_USB_FS0_SYSCFG_SCKE_Msk;
  for (uint32_t i = 0; i < CAN_USB_RAIL_SCKE_TIMEOUT_LOOPS; i++) {
    if (R_USB_FS0->SYSCFG & R_USB_FS0_SYSCFG_SCKE_Msk) {
      break;
    }
  }
  /* An SCKE timeout falls through deliberately: worst case the VDCEN write
   * below is lost and the board parks serving CAN exactly as before this
   * fix — degraded, never dead. */

  R_USB_FS0->USBMC |= R_USB_FS0_USBMC_VDCEN_Msk;

  /* FSP and the HW manual give no VDCEN ramp figure; 5 ms is comfortably
   * above typical LDO start-up plus downstream rail charge. */
  R_BSP_SoftwareDelay(CAN_USB_RAIL_SETTLE_MS, BSP_DELAY_UNITS_MILLISECONDS);
}
#endif /* !CAN_DIAG */

/* CAN update mode (entered via the VBTBKR[4] magic): CAN-only loop; it exits
 * through the GO reset above, an external reset, or (production build) a
 * reset when the driver reports a failed bus-off recovery or after
 * CAN_UPDATE_IDLE_TIMEOUT_MS without an accepted update-protocol frame, so a
 * pulled wire never leaves the board deaf to USB: before
 * ERASE the old app boots again, after ERASE boot_decide's marker check
 * parks it in DFU + CAN. The timebase is board_millis(): board_init()
 * started SysTick (1 ms, tinyusb RA family.c) and enabled IRQs, the same
 * clock main()'s 500 ms double-tap window uses. Ticks lost in the IRQ-off
 * flash windows only lengthen the timeout, and the timer restarts after
 * each handled frame because ERASE blocks this loop for seconds.
 *
 * This entry always follows the application's NVIC_SystemReset(), so the
 * flash open runs against whatever FACI/data-flash state survived that
 * software reset — the
 * gated helper (never a raw DFU_CONFIG_ADDR read) is what keeps a dirty
 * entry state from bus-faulting into a locked-up board. If the open fails,
 * the board still parks and serves CAN on the compiled default IDs. */
static void run_can_update_mode(void)
{
#ifdef CAN_DIAG
  /* Stage 0 of the bench markers (can_diag.c): CAN up on the default IDs
   * BEFORE any flash call. */
  can_initialized = can_diag_entry_bringup();
#else
  /* Power the CAN transceiver rail (USB LDO) BEFORE any CAN register access
   * — see can_xcvr_usb_rail_power_on above for the board-level dependency. */
  can_xcvr_usb_rail_power_on();
#endif

  dfu_config_readable = flash_lp_open_df_read_ready();

  can_load_ids();  /* per-device IDs from the v2 config block (or defaults) */
  can_update_init(&can_state, &can_hal, can_cmd_id);
#ifdef CAN_DIAG
  /* Stages 1-3, then serve CAN with a heartbeat. A diag board never resets
   * off the bench, also when CAN init failed: every init failure leaves the
   * module clocked, so the register polls stay safe. */
  can_initialized = can_diag_entry_report(dfu_config_readable, can_cmd_id,
                                          can_reply_id, can_initialized);
  while (1) {
    (void) can_service_once();
    can_diag_heartbeat(can_reply_id);
  }
#else
  if (can_polled_init(CAN_BITRATE_BPS, can_cmd_id) != CAN_POLLED_OK) {
    NVIC_SystemReset();  /* CAN unusable: reset into the normal boot path */
  }
  can_initialized = true;
  uint32_t last_frame_ms = board_millis();
  while (can_initialized &&
         (board_millis() - last_frame_ms) < CAN_UPDATE_IDLE_TIMEOUT_MS) {
    if (can_service_once()) {
      last_frame_ms = board_millis();
    }
  }
  can_deinit_and_reset();  /* idle timeout, or bus-off recovery failed */
#endif /* CAN_DIAG */
}
#endif /* CAN_LOADER */

/* Status LED. Boards whose Makefile defines LED_FADE_PWM_CHANNEL keep the
 * stock GPT-driven fade; without it (UNO R4 Minima) the LED is a plain GPIO
 * blinking at blink_interval_ms, which saves the GPT driver's ~360 B of the
 * 16 KB region. */
#ifdef LED_FADE_PWM_CHANNEL
#define USEC_PER_SEC	(1000000U)
#define PERIOD 			(USEC_PER_SEC / 50U)
#define FADESTEP		2000

const gpt_extended_cfg_t g_timer_extend = {
  .gtioca = { .output_enabled = LED_FADE_PWM_OUT_A ? true : false, .stop_level = GPT_PIN_LEVEL_LOW },
  .gtiocb = { .output_enabled = LED_FADE_PWM_OUT_B ? true : false, .stop_level = GPT_PIN_LEVEL_LOW },
};


const timer_cfg_t pwm_cfg = {
    .mode = TIMER_MODE_PWM,
    .period_counts = (uint32_t) 0x9896, .duty_cycle_counts = 0x4c4b,
    .source_div =	(timer_source_div_t) LED_TIMER_SOURCE_DIV, .channel = LED_FADE_PWM_CHANNEL, .p_callback = NULL,
    .p_context = NULL, .p_extend = &g_timer_extend, .cycle_end_ipl = (BSP_IRQ_DISABLED),
};


static gpt_instance_ctrl_t pwm_ctrl;
#endif /* LED_FADE_PWM_CHANNEL */
#include "r_ioport.h"

static const ioport_pin_cfg_t extra_pin_cfg[] = {
#ifdef BOSSA_LOADER
  { .pin = BOSSA_TX_PIN, .pin_cfg = (IOPORT_CFG_PERIPHERAL_PIN  | IOPORT_CFG_PULLUP_ENABLE | BOSSA_PERIPHERAL_FLAGS) },
  { .pin = BOSSA_RX_PIN, .pin_cfg = (IOPORT_CFG_PERIPHERAL_PIN  | IOPORT_CFG_PULLUP_ENABLE | BOSSA_PERIPHERAL_FLAGS) },
#endif
#ifdef LED_FADE_PWM_CHANNEL
  { .pin = LED_FADE_GPIO, .pin_cfg = IOPORT_CFG_PERIPHERAL_PIN | IOPORT_PERIPHERAL_GPT1 }
#else
  { .pin = LED_FADE_GPIO, .pin_cfg = IOPORT_CFG_PORT_DIRECTION_OUTPUT | IOPORT_CFG_PORT_OUTPUT_LOW }
#endif
};

static const ioport_cfg_t pin_cfg = {
    .number_of_pins = sizeof(extra_pin_cfg) / sizeof(ioport_pin_cfg_t),
    .p_pin_cfg_data = extra_pin_cfg,
};
ioport_instance_ctrl_t port_ctrl;

#ifdef LED_FADE_PWM_CHANNEL
int pwm_channel = LED_FADE_PWM_OUT_A ? GPT_IO_PIN_GTIOCA : GPT_IO_PIN_GTIOCB;
#endif

/* ***
 * UNO R4 MINIMA AND C33 BOOTLOADER ** ONLY DFU **
 * *** */
__WEAK void run_bootloader() {
  // init device stack on configured roothub port
  tud_init(BOARD_TUD_RHPORT);
  if (board_init_after_tusb) {
    board_init_after_tusb();
  }

  can_diag_stair_probe(4);  /* + tud_init: the full DFU preamble has now run */
  can_diag_stair_end();     /* staircase boot: reset into the boot decision */

#ifdef CAN_LOADER
  /* Recovery/parked mode keeps BOTH transports alive (research §5.2): DFU
   * as stock, plus the CAN update engine polled from the same loop. */
  can_load_ids();  /* per-device IDs from the v2 config block (or defaults) */
  can_update_init(&can_state, &can_hal, can_cmd_id);
  can_initialized = (can_polled_init(CAN_BITRATE_BPS, can_cmd_id) == CAN_POLLED_OK);
#endif

#ifdef CAN_LOADER
  uint32_t can_reinit_ms = board_millis();
#endif
  while (1)
  {
    tud_task(); // tinyusb device task
    led_blinking_task();
    if (dfu_detach_requested) {
      uint32_t start_ms = board_millis();
      while (board_millis() - start_ms < DFU_DETACH_DRAIN_MS) {
        tud_task();  /* complete the DFU_DETACH status stage */
      }
      NVIC_SystemReset();
    }
#ifdef CAN_LOADER
    if (can_initialized) {
      (void) can_service_once();  /* parked: no idle timeout */
    } else if ((board_millis() - can_reinit_ms) >= CAN_REINIT_INTERVAL_MS) {
      can_reinit_ms = board_millis();  /* init failed, or bus-off recovery did */
      can_initialized = (can_polled_init(CAN_BITRATE_BPS, can_cmd_id) == CAN_POLLED_OK);
    }
#endif
  }
}

/*------------- MAIN -------------*/
int main(void)
{
  board_init();

  /* CAN_DIAG builds: once per power cycle, run the DIAG4 staircase along
   * the real preamble below (can_diag.c). */
  if (can_diag_stair_begin()) {
    goto bootloader;
  }

  // set magic for double tap
  if (BOOT_DOUBLE_TAP_DATA == DOUBLE_TAP_MAGIC) {
    set_double_tap_data(0);
    goto bootloader;
  }

#ifdef CAN_LOADER
  // application-requested CAN update mode (mirrors the DFU-detach handoff)
  if (CAN_UPDATE_REQUEST_DATA == CAN_UPDATE_MAGIC) {
    set_can_update_request(0);
    run_can_update_mode();  // never returns; exits via NVIC_SystemReset()
  }
#endif

  if (!R_SYSTEM->RSTSR0_b.PORF) {
	set_double_tap_data(DOUBLE_TAP_MAGIC);
  }
#if !defined(RENESAS_CORTEX_M23) /* no double tap support in Muxto */
#ifdef TURN_OFF_CHARGER_LED
  i2c_begin();
  i2c_write(0x8, 0x9C, (1 << 7));
  i2c_write(0x8, 0x9E, (1 << 5));
#endif
  while (board_millis() < 500) {
  }
#endif
	set_double_tap_data(0);

#ifdef CAN_LOADER
  /* Validate -> decide (rules/bootloader.md; pure logic in
   * src/boot/image_validate.h): a valid CRC record boots. A record failing
   * any check, or the update-in-progress marker without a valid record (an
   * interrupted CAN or DFU update), parks here with DFU + CAN. Only with
   * neither record nor marker does the stock SP-word check apply
   * (documented deviation, for images flashed by tools that write no
   * record, e.g. SWD). */
  int app_valid = (boot_decide((const uint8_t *) 0) == BOOT_DECISION_APP);
#else
  int app_valid = (((*(uint32_t *) SKETCH_FLASH_OFFSET) & 0xFF000000) == 0x20000000);
#endif

  if (app_valid) {
      boot5(SKETCH_FLASH_OFFSET);
  }

bootloader:

  can_diag_stair_probe(0);  /* bare: board_init only (DIAG3's E0 conditions) */

  R_IOPORT_Open(&port_ctrl, &pin_cfg);

  can_diag_stair_probe(1);  /* + IOPORT open */

#ifdef LED_FADE_PWM_CHANNEL
  R_GPT_Open(&pwm_ctrl, &pwm_cfg);
  R_GPT_PeriodSet(&pwm_ctrl, PERIOD);
  R_GPT_DutyCycleSet(&pwm_ctrl, 0, pwm_channel);
  R_GPT_Start(&pwm_ctrl);
#endif

  can_diag_stair_probe(2);  /* + GPT open/period/duty/start (no-op without the fade) */

#if BSP_FEATURE_FLASH_HP_VERSION
  R_FLASH_HP_Open(g_flash.p_ctrl, g_flash.p_cfg);
  R_FLASH_HP_Reset(g_flash.p_ctrl);
  R_FLASH_HP_StartUpAreaSelect(g_flash.p_ctrl, FLASH_STARTUP_AREA_BLOCK0, true);
#endif

#if BSP_FEATURE_FLASH_LP_VERSION
 #ifdef CAN_LOADER
  /* Same Open+Reset as stock, through the shared gate so the CAN service in
   * run_bootloader() only reads DFU_CONFIG_ADDR once provably readable. */
  dfu_config_readable = flash_lp_open_df_read_ready();
 #else
  R_FLASH_LP_Open(g_flash.p_ctrl, g_flash.p_cfg);
  R_FLASH_LP_Reset(g_flash.p_ctrl);
 #endif
#endif

  can_diag_stair_probe(3);  /* + FLASH_LP open/reset (data-flash gate) */

  run_bootloader();

  return 0;
}


#ifdef DFU_LOADER

/* !!!
*
* UNO R4 MINIMA / C33 / OPTA ANALOG / OPTA DIGITAL ->  define this so that they have DFU callbacks
*
* UNO WIFI  does ** NOT ** define this so it avoids DFU callback
*
* !!! */
  

//--------------------------------------------------------------------+
// Device callbacks
//--------------------------------------------------------------------+

// Invoked when device is mounted
void tud_mount_cb(void)
{
  blink_interval_ms = BLINK_MOUNTED;
}

// Invoked when device is unmounted
void tud_umount_cb(void)
{
  blink_interval_ms = BLINK_NOT_MOUNTED;
#ifdef CAN_LOADER
  dfu_owns_flash = false;  /* a download cut off by unplug/reset never completes */
#endif
}

// Invoked when usb bus is suspended
// remote_wakeup_en : if host allow us  to perform remote wakeup
// Within 7ms, device must draw an average of current less than 2.5 mA from bus
void tud_suspend_cb(bool remote_wakeup_en)
{
  (void) remote_wakeup_en;
  blink_interval_ms = BLINK_SUSPENDED;
}

// Invoked when usb bus is resumed
void tud_resume_cb(void)
{
  blink_interval_ms = BLINK_MOUNTED;
}

//--------------------------------------------------------------------+
// DFU callbacks
// Note: alt is used as the partition number, in order to support multiple partitions like FLASH, EEPROM, etc.
//--------------------------------------------------------------------+

// Invoked right before tud_dfu_download_cb() (state=DFU_DNBUSY) or tud_dfu_manifest_cb() (state=DFU_MANIFEST)
// Application return timeout in milliseconds (bwPollTimeout) for the next download/manifest operation.
// During this period, USB host won't try to communicate with us.
uint32_t tud_dfu_get_timeout_cb(uint8_t alt, uint8_t state)
{
  (void) alt;
  (void) state;
  return 0;
}

#ifdef CAN_LOADER
/* ---- USB DFU download into the app slot -----------------------------------
 * Both alt settings target the app slot, as in the stock callbacks below.
 * A session starts only at block 0, which takes the flash owner latch and
 * puts the update-in-progress marker in place (flash_mark_update_pending)
 * before any app byte changes. Blocks must then arrive in order and
 * full-size (CFG_TUD_DFU_XFER_BUFSIZE) except the last, so dfu_received_len
 * alone says where every byte goes and aucbuffer fills to exactly one flash
 * block; nothing may reach METADATA_ADDR. A flash block is erased on its
 * first byte and programmed + read back when full. The manifest programs
 * the buffered tail and writes the record only if the CRC read back from
 * flash equals the running CRC of the bytes received and the vectors pass
 * image_check_bounds. Any failure ends the session with a DFU error status
 * and leaves the marker, so the board parks until an update completes. */
_Static_assert(PROGRAM_BLOCK_SIZE == FLASH_BLOCK_SIZE,
               "aucbuffer must hold exactly one code flash block");
_Static_assert(PROGRAM_BLOCK_SIZE % CFG_TUD_DFU_XFER_BUFSIZE == 0,
               "full-size DFU blocks must tile a flash block");

static uint32_t dfu_received_len;
static uint32_t dfu_received_crc;  /* crc32_ieee_update over the bytes received */

static void dfu_session_fail(uint8_t status)
{
  dfu_owns_flash = false;
  tud_dfu_finish_flashing(status);
}

// Invoked when received DFU_DNLOAD (wLength>0) following by DFU_GETSTATUS (state=DFU_DNBUSY) requests
void tud_dfu_download_cb(uint8_t alt, uint16_t block_num, uint8_t const* data, uint16_t length)
{
  (void) alt;

  if (block_num == 0) {
    dfu_owns_flash = false;  /* block 0 always starts a new session */
    if (can_update_owns_flash(&can_state)) {
      dfu_session_fail(DFU_STATUS_ERR_WRITE);  /* a CAN session holds flash */
      return;
    }
    if (!flash_mark_update_pending()) {
      dfu_session_fail(DFU_STATUS_ERR_ERASE);  /* app slot untouched */
      return;
    }
    dfu_received_len = 0;
    dfu_received_crc = 0;
    dfu_owns_flash = true;
  }

  uint32_t fill = dfu_received_len % PROGRAM_BLOCK_SIZE;
  if (!dfu_owns_flash ||
      dfu_received_len != (uint32_t) block_num * CFG_TUD_DFU_XFER_BUFSIZE ||
      fill + length > sizeof(aucbuffer) ||
      length > APP_SLOT_SIZE - dfu_received_len) {
    dfu_session_fail(DFU_STATUS_ERR_ADDRESS);  /* out of order, after a short block, or too large */
    return;
  }

  uint32_t block_addr = APP_BASE + dfu_received_len - fill;
  if (fill == 0 && !flash_erase_block(block_addr)) {
    dfu_session_fail(DFU_STATUS_ERR_ERASE);
    return;
  }
  memcpy(&aucbuffer[fill], data, length);
  dfu_received_len += length;
  dfu_received_crc = crc32_ieee_update(dfu_received_crc, data, length);
  if (fill + length == sizeof(aucbuffer) &&
      !can_flash_program(block_addr, aucbuffer, sizeof(aucbuffer))) {
    dfu_session_fail(DFU_STATUS_ERR_WRITE);
    return;
  }
  tud_dfu_finish_flashing(DFU_STATUS_OK);
}

/* Program the buffered tail, check what flash holds, write the record. */
static uint8_t dfu_commit_image(void)
{
  if (!dfu_owns_flash) {
    return DFU_STATUS_ERR_NOTDONE;  /* no download, or it already failed */
  }
  uint32_t fill = dfu_received_len % PROGRAM_BLOCK_SIZE;
  if (fill != 0) {
    uint32_t padded = (fill + FLASH_WRITE_UNIT - 1u) & ~(FLASH_WRITE_UNIT - 1u);
    memset(&aucbuffer[fill], 0xFF, padded - fill);
    if (!can_flash_program(APP_BASE + dfu_received_len - fill, aucbuffer, padded)) {
      return DFU_STATUS_ERR_WRITE;
    }
  }
  const uint8_t *app = (const uint8_t *) APP_BASE;
  if (crc32_ieee(app, dfu_received_len) != dfu_received_crc) {
    return DFU_STATUS_ERR_VERIFY;
  }
  if (image_check_bounds(app, dfu_received_len) != IMAGE_VALID) {
    return DFU_STATUS_ERR_FIRMWARE;
  }
  _Alignas(FLASH_WRITE_UNIT) uint8_t meta[IMAGE_META_SIZE];
  image_meta_encode(meta, dfu_received_len, dfu_received_crc);
  return can_flash_program(METADATA_ADDR, meta, sizeof(meta)) ? DFU_STATUS_OK
                                                               : DFU_STATUS_ERR_WRITE;
}

// Invoked when download process is complete, received DFU_DNLOAD (wLength=0) following by DFU_GETSTATUS (state=Manifest)
void tud_dfu_manifest_cb(uint8_t alt)
{
  (void) alt;
  uint8_t status = dfu_commit_image();
  dfu_owns_flash = false;
  /* No R_FLASH_LP_Close(): the descriptor is manifestation-tolerant, so the
   * board stays parked here, and later CAN or DFU updates need the driver. */
  tud_dfu_finish_flashing(status);
}

// Invoked when received DFU_UPLOAD request: reads stop at the end of the app slot
uint16_t tud_dfu_upload_cb(uint8_t alt, uint16_t block_num, uint8_t* data, uint16_t length)
{
  (void) alt;
  uint32_t offset = (uint32_t) block_num * length;
  if (offset >= APP_SLOT_SIZE) {
    return 0;
  }
  uint32_t count = (APP_SLOT_SIZE - offset < length) ? (APP_SLOT_SIZE - offset) : length;
  memcpy(data, (const uint8_t *) APP_BASE + offset, count);
  return (uint16_t) count;
}

// Invoked when the Host has terminated a download or upload transfer
void tud_dfu_abort_cb(uint8_t alt)
{
  (void) alt;
  dfu_owns_flash = false;  /* the marker stays: an unfinished download parks */
}

#else /* stock callbacks */

#ifndef ZEPHYR_SKETCH_FLASH_OFFSET
#define ZEPHYR_SKETCH_FLASH_OFFSET SKETCH_FLASH_OFFSET
#endif
static uint32_t _offset;

#if BSP_FEATURE_FLASH_HP_VERSION
int flash_write_block() {
  return R_FLASH_HP_Write(g_flash.p_ctrl, (uint32_t)aucbuffer, _offset + ((auc_block_num * CFG_TUD_DFU_XFER_BUFSIZE)/sizeof(aucbuffer))*sizeof(aucbuffer) , auclen);
}
#endif

#if BSP_FEATURE_FLASH_LP_VERSION
int flash_write_block() {
  return R_FLASH_LP_Write(g_flash.p_ctrl, (uint32_t)aucbuffer, _offset + ((auc_block_num * CFG_TUD_DFU_XFER_BUFSIZE)/sizeof(aucbuffer))*sizeof(aucbuffer) , PROGRAM_BLOCK_SIZE);
}
#endif

// Invoked when received DFU_DNLOAD (wLength>0) following by DFU_GETSTATUS (state=DFU_DNBUSY) requests
// This callback could be returned before flashing op is complete (async).
// Once finished flashing, application must call tud_dfu_finish_flashing()
void tud_dfu_download_cb(uint8_t alt, uint16_t block_num, uint8_t const* data, uint16_t length)
{
  _offset = alt == 0 ? SKETCH_FLASH_OFFSET : ZEPHYR_SKETCH_FLASH_OFFSET;
  auc_block_num = block_num;

  //printf("\r\nReceived Alt %u BlockNum %u of length %u\r\n", alt, block_num, length);

  if ((_offset + (block_num * CFG_TUD_DFU_XFER_BUFSIZE)) % PROGRAM_BLOCK_SIZE == 0) {
    // erase block
    __disable_irq();
#if BSP_FEATURE_FLASH_HP_VERSION
    int err = R_FLASH_HP_Erase(g_flash.p_ctrl, _offset + (block_num * CFG_TUD_DFU_XFER_BUFSIZE), 1);
#endif
#if BSP_FEATURE_FLASH_LP_VERSION
    int err = R_FLASH_LP_Erase(g_flash.p_ctrl, _offset + (block_num * CFG_TUD_DFU_XFER_BUFSIZE), 1);
#endif
    __enable_irq();
  }
  memcpy(&aucbuffer[auclen], data, length);
  auclen += length;

  if (auclen == sizeof(aucbuffer)) {
    __disable_irq();
    flash_write_block();
    __enable_irq();
    auclen = 0;
  }

  tud_dfu_finish_flashing(DFU_STATUS_OK);

  // flashing op for download complete without error
}

// Invoked when download process is complete, received DFU_DNLOAD (wLength=0) following by DFU_GETSTATUS (state=Manifest)
// Application can do checksum, or actual flashing if buffered entire image previously.
// Once finished flashing, application must call tud_dfu_finish_flashing()
void tud_dfu_manifest_cb(uint8_t alt)
{
  (void) alt;
  //printf("Download completed, enter manifestation\r\n");

  __disable_irq();
  flash_write_block();
  __enable_irq();

  // flashing op for manifest is complete without error
  // Application can perform checksum, should it fail, use appropriate status such as errVERIFY.
  tud_dfu_finish_flashing(DFU_STATUS_OK);

  // Try booting the application
  // boot5(SKETCH_FLASH_OFFSET);
#if BSP_FEATURE_FLASH_HP_VERSION
    R_FLASH_HP_Close(g_flash.p_ctrl);
#endif
#if BSP_FEATURE_FLASH_LP_VERSION
    R_FLASH_LP_Close(g_flash.p_ctrl);
#endif
}

// Invoked when received DFU_UPLOAD request
// Application must populate data with up to length bytes and
// Return the number of written bytes
uint16_t tud_dfu_upload_cb(uint8_t alt, uint16_t block_num, uint8_t* data, uint16_t length)
{
  _offset = alt == 0 ? SKETCH_FLASH_OFFSET : ZEPHYR_SKETCH_FLASH_OFFSET;

  memcpy(data, _offset + (void*)(block_num * length), length);

  return length;
}

// Invoked when the Host has terminated a download or upload transfer
void tud_dfu_abort_cb(uint8_t alt)
{
  (void) alt;
  //printf("Host aborted transfer\r\n");
}
#endif /* CAN_LOADER */

// Invoked when a DFU_DETACH request is received. This runs inside
// tud_task(): the drain and reset wait for the main loop (run_bootloader).
void tud_dfu_detach_cb(void)
{
  dfu_detach_requested = true;
}

#endif

//--------------------------------------------------------------------+
// BLINKING TASK + Indicator pulse
//--------------------------------------------------------------------+

#ifdef LED_FADE_PWM_CHANNEL
void pulse_led()
{
	static uint32_t pulse_width;
	static uint8_t dir;

  R_GPT_DutyCycleSet(&pwm_ctrl, pulse_width, pwm_channel);

	if (dir) {
		if (pulse_width < FADESTEP) {
			dir = 0U;
			pulse_width = FADESTEP;
		} else {
			pulse_width -= FADESTEP;
		}
	} else {
		pulse_width += FADESTEP;

		if (pulse_width >= PERIOD) {
			dir = 1U;
			pulse_width = PERIOD;
		}
	}
}

void led_blinking_task(void)
{
  static uint32_t start_ms = 0;

  // Blink every interval ms
  if ( board_millis() - start_ms < 100) return;
  start_ms += 100;

  pulse_led();
}
#else
/* Toggle the LED every blink_interval_ms: 250 ms with no USB host (the usual
 * parked state on a CAN-only node), 1 s mounted, 2.5 s suspended. */
void led_blinking_task(void)
{
  static uint32_t start_ms;
  static bsp_io_level_t led_level = BSP_IO_LEVEL_LOW;

  if (board_millis() - start_ms < blink_interval_ms) return;
  start_ms = board_millis();

  led_level = (led_level == BSP_IO_LEVEL_LOW) ? BSP_IO_LEVEL_HIGH : BSP_IO_LEVEL_LOW;
  (void) R_IOPORT_PinWrite(&port_ctrl, LED_FADE_GPIO, led_level);
}
#endif /* LED_FADE_PWM_CHANNEL */

//--------------------------------------------------------------------+
// Double tap magic
//--------------------------------------------------------------------+
void set_double_tap_data(uint32_t data)
{
  R_SYSTEM->PRCR = (uint16_t) BSP_PRV_PRCR_PRC1_UNLOCK;
  BOOT_DOUBLE_TAP_DATA = data;
  R_SYSTEM->PRCR = (uint16_t) BSP_PRV_PRCR_LOCK;
}
