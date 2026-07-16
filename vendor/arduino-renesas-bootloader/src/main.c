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
#include "r_gpt.h"

#include "flash.h"

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

uint8_t aucbuffer[PROGRAM_BLOCK_SIZE];
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

/* Instance structure to use this module. */
const flash_instance_t g_flash ={ .p_ctrl = &g_flash_ctrl, .p_cfg = &g_flash_cfg, .p_api = &flash_apis };

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

void led_blinking_task(void);
void set_double_tap_data(uint32_t data);

void boot5(uint32_t address) {

#ifdef CAN_LOADER
  /* Jump rule (rules/bootloader.md): deinit every peripheral the bootloader
   * touched. No current caller reaches here with CAN up, but keep the jump
   * sequence self-contained. */
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

  __disable_irq(); // Note: remember to enable IRQ in application
  __DSB();
  __ISB();

  // Disable SysTick
  SysTick->CTRL = 0;

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

static bool dfu_config_readable = false;

#ifdef CAN_DIAG
/* ---- CAN_DIAG bench instrumentation (throwaway) --------------------------
 * Stage-marker frames {0xD1, stage, ...} emitted during the magic-entry path
 * (run_can_update_mode) so a bench candump shows exactly where entry dies.
 * The real update engine stays fully functional. Nothing in any CAN_DIAG
 * block compiles in a CAN_DIAG=0 build (byte-identical). One deliberate
 * SUBTRACTION: the production USB-rail power-up (can_xcvr_usb_rail_power_on)
 * is compiled OUT of CAN_DIAG builds, so the staircase keeps reproducing the
 * unpowered-rail failure it diagnosed and the diag image stays inside the
 * 16 KiB gate. */
#define CAN_DIAG_MARKER              0xD1u
#define CAN_DIAG_STAGE_CAN_UP        0x00u  /* after default-ID can_polled_init  */
#define CAN_DIAG_STAGE_FLASH_GATE    0x01u  /* after flash_lp_open_df_read_ready */
#define CAN_DIAG_STAGE_IDS_LOADED    0x02u  /* after can_load_ids                */
#define CAN_DIAG_STAGE_REFILTER      0x03u  /* after re-init on the loaded ID    */
#define CAN_DIAG_STAGE_STAIR_RESULT  0xE4u  /* DIAG4 staircase verdict frame     */
#define CAN_DIAG_STAGE_STAIR_LOG     0xE5u  /* DIAG4 failed-attempt register log */
#define CAN_DIAG_STAGE_HEARTBEAT     0xFBu
#define CAN_DIAG_NO_REINIT           0xEEu  /* stage-3 status: IDs = defaults    */
/* Crude ~500 ms heartbeat period by busy loop count: no SysTick guaranteed in
 * this loop, and no interrupts allowed. One empty-FIFO service pass is a
 * handful of register reads at 48 MHz — order-of-magnitude is all we need. */
#define CAN_DIAG_HEARTBEAT_LOOPS     500000u

/* DIAG3 retry-bisect knobs. One attempt is bounded by the driver's ~200 ms
 * SENTDATA wait (twice, with the second-send probe) plus the 100 ms settle,
 * so the 3 s budget dominates when sends time out slowly and the attempt cap
 * when they fail fast; both together keep the loop finite even if SysTick
 * (board_millis) were dead. */
#define CAN_DIAG_RETRY_MAX_ATTEMPTS  30u
#define CAN_DIAG_RETRY_BUDGET_MS     3000u
#define CAN_DIAG_RETRY_SETTLE_MS     100u
#define CAN_DIAG_SECOND_SEND         0x80u  /* b2 flag: 2nd TX of the attempt */

static uint16_t can_diag_open_err;   /* fsp_err_t of R_FLASH_LP_Open  */
static uint16_t can_diag_reset_err;  /* fsp_err_t of R_FLASH_LP_Reset */

static void can_diag_send(uint32_t id, uint8_t stage, uint8_t b2, uint8_t b3,
                          uint8_t b4, uint8_t b5, uint8_t b6, uint8_t b7)
{
  const uint8_t frame[8] = { CAN_DIAG_MARKER, stage, b2, b3, b4, b5, b6, b7 };
  (void) can_polled_send(id, 8, frame);  /* diag marker: loss is acceptable */
}

/* ---- DIAG3: retry-based bring-up bisect ----------------------------------
 * DIAG2 established that one init+send right after board_init() NEVER
 * transmits while the identical calls later in the DFU path do. One attempt
 * here = can_polled_init -> marker send -> (on failure) ONE more send on the
 * SAME init -> deinit + 100 ms settle -> retry, until a frame is ACKed on
 * the wire or the budget runs out. The first frame that arrives carries the
 * whole story:
 *   b2  attempt number (1-based); bit7 SET = the frame went out on the
 *       SECOND send of its attempt — init was fine and only the first TX
 *       after bring-up is lost (first-TX quirk, not settle time)
 *   b3  init status of the winning attempt
 *   b4  previous attempt's last send status (0xFF = none, first attempt)
 *   b5  RSTSR1 low byte (bit2 SWRF = software-reset entry), b6 RSTSR2
 *   b7  previous attempt's TECR, captured right after its failed send and
 *       before deinit: 0 = the controller never drove the bus (mode/sync
 *       problem); >= 8 and climbing toward 128+ = it DID transmit but every
 *       frame died on the wire (bit-timing/ACK problem)
 * On total failure the last attempt's init is deliberately left in place
 * (module clocked, registers safe to poll) and *out_sent stays false. */
static can_polled_status_t can_diag_bringup_retry(uint8_t stage, bool *out_sent)
{
  uint8_t rstsr1_lo = (uint8_t) R_SYSTEM->RSTSR1;
  uint8_t rstsr2    = (uint8_t) R_SYSTEM->RSTSR2;
  uint8_t prev_send = 0xFFu;                       /* no previous attempt */
  uint8_t prev_tecr = 0;
  can_polled_status_t init_status = CAN_POLLED_ERROR;
  uint32_t start_ms = board_millis();

  *out_sent = false;
  for (uint8_t attempt = 1; ; attempt++) {
    init_status = can_polled_init(CAN_BITRATE_BPS, CAN_ID_HOST_TO_BOOT);

    uint8_t frame[8] = { CAN_DIAG_MARKER, stage, attempt, (uint8_t) init_status,
                         prev_send, rstsr1_lo, rstsr2, prev_tecr };
    can_polled_status_t send_status = init_status;
    if (init_status == CAN_POLLED_OK) {
      send_status = can_polled_send(CAN_ID_BOOT_TO_HOST, 8, frame);
      if (send_status != CAN_POLLED_OK) {
        /* Priming probe: retry on the SAME init, no delay, no reinit. */
        frame[2] = (uint8_t) (attempt | CAN_DIAG_SECOND_SEND);
        send_status = can_polled_send(CAN_ID_BOOT_TO_HOST, 8, frame);
      }
    }
    if (send_status == CAN_POLLED_OK) {
      *out_sent = true;
      return init_status;
    }
    if ((attempt >= CAN_DIAG_RETRY_MAX_ATTEMPTS) ||
        ((board_millis() - start_ms) >= CAN_DIAG_RETRY_BUDGET_MS)) {
      return init_status;  /* leave the last init up: register polls stay safe */
    }
    prev_send = (uint8_t) send_status;
    prev_tecr = R_CAN0->TECR;      /* before deinit's reset clears the counter */
    can_polled_deinit();
    R_BSP_SoftwareDelay(CAN_DIAG_RETRY_SETTLE_MS, BSP_DELAY_UNITS_MILLISECONDS);
  }
}
#endif /* CAN_DIAG */

static bool flash_lp_open_df_read_ready(void)
{
#ifdef CAN_DIAG
  /* Same calls/short-circuits as stock, with the fsp_err_t values latched
   * for the stage-1 marker frame. */
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
 * src/boot/can_update.h; in short: the metadata block is erased first and
 * rewritten last, so an interruption at any instant leaves the image
 * invalid and the device parked in this bootloader. The bootloader's own
 * 8 flash blocks (0x0000-0x3FFF) are never touched. */

static bool can_flash_erase_app_and_metadata(void)
{
  /* Metadata block FIRST: invalidates the old image before any app byte is
   * disturbed (fail closed). */
  __disable_irq();
  int err = R_FLASH_LP_Erase(g_flash.p_ctrl, METADATA_ADDR, 1);
  __enable_irq();
  if (err != FSP_SUCCESS) {
    return false;
  }

  /* App slot, one block per IRQ-off window so USB stays serviceable when
   * CAN runs inside the DFU loop. */
  for (uint32_t block = 0; block < (APP_SLOT_SIZE / FLASH_BLOCK_SIZE); block++) {
    __disable_irq();
    err = R_FLASH_LP_Erase(g_flash.p_ctrl, APP_BASE + block * FLASH_BLOCK_SIZE, 1);
    __enable_irq();
    if (err != FSP_SUCCESS) {
      return false;
    }
  }
  return true;
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

static const can_update_hal_t can_hal = {
  .flash_erase_app_and_metadata = can_flash_erase_app_and_metadata,
  .flash_program                = can_flash_program,
  .flash_base                   = (const uint8_t *) 0,  /* code flash is mapped at 0 */
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

/* INFO (host CMD 0x07): report the device's USB product name so a host can
 * identify a board parked in the bootloader. Answered here, not in the update
 * engine, because the reply is 5 frames (the engine emits one per frame).
 *
 * Reply on CAN_ID_BOOT_TO_HOST (frozen contract):
 *   frame 0  {0x79, 0x07, name_len, protocol_version, 0xFF, 0xFF, 0xFF, 0xFF}
 *   frames 1..4  the 32-byte product-name field, 8 bytes each in order,
 *                ASCII, 0x00-padded beyond name_len.
 *
 * Name source: dfu_config_parse of the 76-byte block at DFU_CONFIG_ADDR (data
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
  (void) can_polled_send(can_reply_id, 8, frame);

  for (uint32_t off = 0; off < DFU_CONFIG_STR_MAX; off += 8u) {
    (void) can_polled_send(can_reply_id, 8, &name[off]);
  }
}

/* Poll one frame through the update engine, transmit its reply, and perform
 * the GO reset. Shared by the CAN-only mode and the combined DFU+CAN loop. */
static void can_service_once(void)
{
  /* Zero-init: out-params are written by the callees before use, but gcc 7
   * LTO cannot prove it and -Werror=maybe-uninitialized trips otherwise. */
  uint32_t id = 0;
  uint8_t dlc = 0;
  uint8_t data[8] = {0};
  uint8_t reply[8] = {0};
  can_update_action_t action = CAN_UPDATE_ACTION_NONE;

  if (can_polled_receive(&id, &dlc, data) != CAN_POLLED_OK) {
    return;  /* empty FIFO, or bus-off (driver re-initialized itself) */
  }

  /* INFO is intercepted before the update engine: its 5-frame reply does not
   * fit the engine's one-reply-per-frame contract. Answering it does not touch
   * or advance the update session state. */
  if (id == can_cmd_id && data[0] == CAN_UPDATE_CMD_INFO) {
    can_service_info();
    return;
  }

  if (can_update_handle_frame(&can_state, id, dlc, data, reply, &action)) {
    (void) can_polled_send(can_reply_id, 8, reply);  /* host retries on loss */
  }
  if (action == CAN_UPDATE_ACTION_RESET) {
    can_polled_deinit();  /* deinit every peripheral this path touched */
    can_initialized = false;
    NVIC_SystemReset();   /* clean re-entry through the boot decision */
  }
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
 * pad. Compiled OUT of CAN_DIAG builds — the staircase must keep reproducing
 * the unpowered-rail conditions it diagnosed, and the diag image is 32 B
 * under the 16 KiB gate. */
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

/* CAN update mode (entered via the VBTBKR[4] magic): CAN-only loop; the only
 * exits are the GO reset above or an external reset. This entry always
 * follows the application's NVIC_SystemReset(), so the flash open runs
 * against whatever FACI/data-flash state survived that software reset — the
 * gated helper (never a raw DFU_CONFIG_ADDR read) is what keeps a dirty
 * entry state from bus-faulting into a locked-up board. If the open fails,
 * the board still parks and serves CAN on the compiled default IDs. */
static void run_can_update_mode(void)
{
#ifdef CAN_DIAG
  /* Stage 0 (DIAG3): retry-based CAN bring-up on the COMPILED default IDs
   * BEFORE any flash call — the real bisect target. The winning frame's
   * attempt count quantifies the settle time this path needs; if every
   * attempt fails the board still parks and serves the CAN loop below on the
   * last init (deliberate: a diag board never resets off the bench). */
  bool stage0_sent;
  can_polled_status_t init_status = can_diag_bringup_retry(CAN_DIAG_STAGE_CAN_UP,
                                                           &stage0_sent);
  can_initialized = (init_status == CAN_POLLED_OK);
  (void) stage0_sent;  /* the bench reads success off the wire, not this flag */

  /* Stage 1: the data-flash read gate. byte7 = DFLCTL as found on entry —
   * directly tests the "DFLEN survived the software reset" hypothesis the
   * gate comment above documents. */
  uint8_t dflctl_on_entry = (uint8_t) R_FACI_LP->DFLCTL;
  dfu_config_readable = flash_lp_open_df_read_ready();
  can_diag_send(CAN_ID_BOOT_TO_HOST, CAN_DIAG_STAGE_FLASH_GATE,
                dfu_config_readable ? 1u : 0u,
                (uint8_t) can_diag_open_err, (uint8_t) (can_diag_open_err >> 8),
                (uint8_t) can_diag_reset_err, (uint8_t) (can_diag_reset_err >> 8),
                dflctl_on_entry);

  /* Stage 2: per-device IDs from the v2 config block (or defaults). */
  can_load_ids();
  can_diag_send(CAN_ID_BOOT_TO_HOST, CAN_DIAG_STAGE_IDS_LOADED,
                dfu_config_readable ? 1u : 0u,
                (uint8_t) can_cmd_id, (uint8_t) (can_cmd_id >> 8),
                (uint8_t) can_reply_id, (uint8_t) (can_reply_id >> 8), 0);

  /* Stage 3: re-filter onto the loaded cmd ID when it differs from the
   * default; the marker goes out on the LOADED reply ID so the bench proves
   * that ID actually transmits. */
  uint8_t refilter_status = CAN_DIAG_NO_REINIT;
  if (can_cmd_id != CAN_ID_HOST_TO_BOOT) {
    can_polled_deinit();
    init_status = can_polled_init(CAN_BITRATE_BPS, can_cmd_id);
    can_initialized = (init_status == CAN_POLLED_OK);
    refilter_status = (uint8_t) init_status;
  }
  can_diag_send(can_reply_id, CAN_DIAG_STAGE_REFILTER, refilter_status,
                0, 0, 0, 0, 0);

  /* Engine matches on whatever cmd ID is now in effect (stock semantics).
   * Deliberate diag deviation: a failed CAN init parks here instead of
   * resetting into the normal boot path — a diag board must stay on the
   * bench, and every init failure path above leaves the CAN module clocked
   * (module-stop is released before any failing step), so the register
   * polls below stay safe. */
  can_update_init(&can_state, &can_hal, can_cmd_id);

  uint32_t hb_loops = 0;
  uint8_t hb_seq = 0;
  while (1) {
    can_service_once();
    if (++hb_loops >= CAN_DIAG_HEARTBEAT_LOOPS) {
      hb_loops = 0;
      can_diag_send(can_reply_id, CAN_DIAG_STAGE_HEARTBEAT, hb_seq, 0, 0, 0, 0, 0);
      /* Every 4th heartbeat also on the default reply ID, so the board is
       * provably alive no matter which reply ID the bench watches. */
      if (((hb_seq & 3u) == 0u) && (can_reply_id != CAN_ID_BOOT_TO_HOST)) {
        can_diag_send(CAN_ID_BOOT_TO_HOST, CAN_DIAG_STAGE_HEARTBEAT, hb_seq, 0, 0, 0, 0, 0);
      }
      hb_seq++;
    }
  }
#else
  /* Power the CAN transceiver rail (USB LDO) BEFORE any CAN register access
   * — see can_xcvr_usb_rail_power_on above for the board-level dependency. */
  can_xcvr_usb_rail_power_on();

  dfu_config_readable = flash_lp_open_df_read_ready();

  can_load_ids();  /* per-device IDs from the v2 config block (or defaults) */
  can_update_init(&can_state, &can_hal, can_cmd_id);
  if (can_polled_init(CAN_BITRATE_BPS, can_cmd_id) != CAN_POLLED_OK) {
    NVIC_SystemReset();  /* CAN unusable: reset into the normal boot path */
  }
  can_initialized = true;
  while (1) {
    can_service_once();
  }
#endif /* CAN_DIAG */
}
#endif /* CAN_LOADER */

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
#include "r_ioport.h"

static const ioport_pin_cfg_t extra_pin_cfg[] = {
#ifdef BOSSA_LOADER
  { .pin = BOSSA_TX_PIN, .pin_cfg = (IOPORT_CFG_PERIPHERAL_PIN  | IOPORT_CFG_PULLUP_ENABLE | BOSSA_PERIPHERAL_FLAGS) },
  { .pin = BOSSA_RX_PIN, .pin_cfg = (IOPORT_CFG_PERIPHERAL_PIN  | IOPORT_CFG_PULLUP_ENABLE | BOSSA_PERIPHERAL_FLAGS) },
#endif
  { .pin = LED_FADE_GPIO, .pin_cfg = IOPORT_CFG_PERIPHERAL_PIN | IOPORT_PERIPHERAL_GPT1 }
};

static const ioport_cfg_t pin_cfg = {
    .number_of_pins = sizeof(extra_pin_cfg) / sizeof(ioport_pin_cfg_t),
    .p_pin_cfg_data = extra_pin_cfg,
};
ioport_instance_ctrl_t port_ctrl;

int pwm_channel = LED_FADE_PWM_OUT_A ? GPT_IO_PIN_GTIOCA : GPT_IO_PIN_GTIOCB;

#if defined(CAN_LOADER) && defined(CAN_DIAG)
/* ---- DIAG4: one-boot self-bisecting staircase -----------------------------
 * DIAG3 (hardware-confirmed via SWD readback) ruled timing out: 30 bare
 * init+send attempts over 3 s right after board_init() never put a frame on
 * a live, ACKing bus — while the IDENTICAL driver calls after the DFU
 * preamble (IOPORT open -> GPT open/start -> FLASH_LP open/reset ->
 * tud_init) always transmit. So at least one preamble piece is a hard
 * functional prerequisite for CAN0 TX. The staircase finds WHICH in one
 * boot. Bench verdict (2026-07, real bus): step 4 (+tud_init) won on its
 * first attempt after 12 failures across steps 0-3, every one TX-timeout
 * with TEC = 0 in operate mode -> the USB LDO (USBMC.VDCEN) powers the
 * external CAN transceiver rail; production fix in
 * can_xcvr_usb_rail_power_on. The staircase does NOT duplicate the preamble
 * calls (a first cut did, and
 * LTO's constprop clones of tud_init/R_GPT_Open/R_GPT_PeriodSet cost 636 B
 * the 16 KiB region does not have). Instead, the staircase boot jumps into
 * the REAL `bootloader:` path with a can_diag_stair_probe() interleaved
 * after each preamble piece, gated on can_diag_stair_active, and ends in a
 * reset before the DFU loop. Fidelity is exact by construction: the pieces
 * that run are the production calls themselves, in production order.
 *
 * Each probe: can_polled_init on the compiled default IDs, then up to
 * CAN_DIAG_STAIR_SEND_TRIES sends. Every attempt's payload IS the verdict
 * frame, so the first frame on the bus names the winner:
 *   {0xD1, 0xE4, step, attempt, STR lo, STR hi, TECR, ECSR}
 *   step: 0 = bare (would contradict DIAG3), 1 = +IOPORT, 2 = +GPT,
 *         3 = +FLASH_LP open/reset, 4 = +tud_init, 0xFF = nothing worked
 *   (registers sampled after init, just before that attempt's TRMREQ).
 * The success latch stops later probes, so exactly one step ever reports.
 *
 * Every FAILED attempt records {STR, ECSR, TECR} into a one-entry "last
 * failure before success" snapshot, emitted right after the verdict:
 *   {0xD1, 0xE5, idx, STR lo, STR hi, ECSR, TECR, kind}
 *   idx = number of failed attempts preceding the success (0 = none -> no
 *   E5 frame). idx maps back to (step, attempt) because attempts run in
 *   fixed step order, SEND_TRIES per step. STR RSTST/HLTST/SLPST (bits
 *   8/9/10) = mode the controller was REALLY in; TECR = 0 -> it never
 *   drove the bus (mode/clock problem), climbing -> frames died on the
 *   wire; ECSR error-code flags (ADEF/ACKEF/SEF/FEF/AEF/BE0F/BE1F) say HOW
 *   they died — ACKEF = nobody ACKed, stuff/bit errors = bit-timing wrong.
 *   kind: 0xA5 = TX timed out (registers sampled after the driver's abort;
 *   MCTL0 latch evidence not captured), 0xEE = can_polled_init failed.
 * Additionally, attempts 2..SEND_TRIES of any step carry post-failure
 * TECR/ECSR/STR in their OWN E4 payload (same init, sampled pre-send), so
 * a step that wins on attempt >= 2 documents its own failures.
 * (DIAG4 size cuts, in order applied: [1] the spec'd 8-entry per-attempt
 * register log -> one-entry last-failure snapshot; [2] the raw-MCTL TX
 * probe (captured MCTL[0] before the driver's abort-clear) -> driver sends,
 * losing the "did TRMREQ/SENTDATA latch" byte. Both forced by the 16 KiB
 * region: full design measured 16,604 B, snapshot-only 16,472 B, this
 * layout fits. The 0xEE/0xA5 kind byte preserves the E5 frame format.)
 *
 * Run-once / cannot-boot-loop design: VBTBKR[8..11] (VBATT backup domain,
 * the proven mechanism behind the double-tap and CAN-update magics) holds a
 * "staircase done" word. Garbage on the first boot after power-up -> run
 * the staircase, but write the flag FIRST so even a crash mid-staircase
 * cannot re-enter it, then end in one NVIC_SystemReset so the app/DFU/CAN
 * boot decisions always start from clean peripherals (GPT/USB/flash state
 * from the staircase never leaks into the app). If the flag readback ever
 * failed to stick, the board parks in the normal DFU+CAN service loop
 * instead of resetting — degraded but bounded: no reset loop, no brick.
 * Later boots skip every probe until the next power cycle. A pending
 * double-tap or CAN-update magic is left untouched by the staircase boot
 * and honored one boot (~1 s) later. */
#define CAN_DIAG_STAIR_DONE_DATA   (*((volatile uint32_t *) (uintptr_t) &R_SYSTEM->VBTBKR[8]))
#define CAN_DIAG_STAIR_DONE_MAGIC  0x5D1A6435u
#define CAN_DIAG_STAIR_SEND_TRIES  3u
#define CAN_DIAG_STAIR_STEP_NONE   0xFFu
#define CAN_DIAG_STAIR_TX_FAILED   0xA5u    /* kind: send timed out          */
#define CAN_DIAG_STAIR_INIT_FAILED 0xEEu    /* kind: init failed, no TX done */

static bool can_diag_stair_active;          /* this boot is the staircase boot */
static bool can_diag_stair_won;             /* success latch: stop later probes */

/* One struct, one base address: five separate statics cost five literal-pool
 * entries and the diag build is 16 B from the region limit. */
static struct {
  uint16_t str;
  uint8_t  ecsr;
  uint8_t  tecr;
  uint8_t  kind;
  uint8_t  count;
} can_diag_stair_fail;

static void can_diag_stair_log_add(uint8_t kind)
{
  can_diag_stair_fail.count++;
  can_diag_stair_fail.str  = R_CAN0->STR;
  can_diag_stair_fail.ecsr = R_CAN0->ECSR;
  can_diag_stair_fail.tecr = R_CAN0->TECR;
  can_diag_stair_fail.kind = kind;
}

/* One staircase level: bring CAN up, try to get the verdict frame for
 * `step` onto the bus, tear CAN down again (the next preamble piece must
 * run with CAN down, exactly like the production path). No-op on non-
 * staircase boots and after a step has already won. */
static void can_diag_stair_probe(uint8_t step)
{
  if (!can_diag_stair_active || can_diag_stair_won) {
    return;
  }
  if (can_polled_init(CAN_BITRATE_BPS, CAN_ID_HOST_TO_BOOT) != CAN_POLLED_OK) {
    can_diag_stair_log_add(CAN_DIAG_STAIR_INIT_FAILED);
    can_polled_deinit();
    return;
  }
  for (uint8_t attempt = 1; attempt <= CAN_DIAG_STAIR_SEND_TRIES; attempt++) {
    /* The attempt's payload IS the E4 verdict frame; registers sampled just
     * before the send, so attempts >= 2 carry the previous failure's TECR/
     * ECSR (same init — can_polled_send's abort clears no error state). */
    uint16_t str = R_CAN0->STR;
    const uint8_t frame[8] = { CAN_DIAG_MARKER, CAN_DIAG_STAGE_STAIR_RESULT,
                               step, attempt, (uint8_t) str, (uint8_t) (str >> 8),
                               R_CAN0->TECR, R_CAN0->ECSR };
    if (can_polled_send(CAN_ID_BOOT_TO_HOST, 8, frame) == CAN_POLLED_OK) {
      if (can_diag_stair_fail.count != 0) {
        can_diag_send(CAN_ID_BOOT_TO_HOST, CAN_DIAG_STAGE_STAIR_LOG,
                      can_diag_stair_fail.count,
                      (uint8_t) can_diag_stair_fail.str,
                      (uint8_t) (can_diag_stair_fail.str >> 8),
                      can_diag_stair_fail.ecsr, can_diag_stair_fail.tecr,
                      can_diag_stair_fail.kind);
      }
      can_diag_stair_won = true;
      break;
    }
    can_diag_stair_log_add(CAN_DIAG_STAIR_TX_FAILED);
  }
  can_polled_deinit();
}

static void can_diag_stair_set_done(void)
{
  R_SYSTEM->PRCR = (uint16_t) BSP_PRV_PRCR_PRC1_UNLOCK;
  CAN_DIAG_STAIR_DONE_DATA = CAN_DIAG_STAIR_DONE_MAGIC;
  R_SYSTEM->PRCR = (uint16_t) BSP_PRV_PRCR_LOCK;
}
#endif /* CAN_LOADER && CAN_DIAG */

/* ***
 * UNO R4 MINIMA AND C33 BOOTLOADER ** ONLY DFU **
 * *** */
__WEAK void run_bootloader() {
  // init device stack on configured roothub port
  tud_init(BOARD_TUD_RHPORT);
  if (board_init_after_tusb) {
    board_init_after_tusb();
  }

#if defined(CAN_LOADER) && defined(CAN_DIAG)
  can_diag_stair_probe(4);  /* + tud_init: the full DFU preamble has now run */
  if (can_diag_stair_active) {
    /* Nothing worked? One last round (step 0xFF) so the verdict + snapshot
     * get a chance on the fully-brought-up board; a no-op if a step won. */
    can_diag_stair_probe(CAN_DIAG_STAIR_STEP_NONE);
    if (CAN_DIAG_STAIR_DONE_DATA == CAN_DIAG_STAIR_DONE_MAGIC) {
      NVIC_SystemReset();  /* staircase boot ends: clean re-entry for the
                            * real boot decision, run-once flag is set */
    }
    /* Flag write did not stick (never observed: the double-tap word proves
     * VBTBKR works). Resetting would loop forever — park in the normal
     * DFU+CAN service loop below instead. */
    can_diag_stair_active = false;
  }
#endif

#ifdef CAN_LOADER
  /* Recovery/parked mode keeps BOTH transports alive (research §5.2): DFU
   * as stock, plus the CAN update engine polled from the same loop. */
  can_load_ids();  /* per-device IDs from the v2 config block (or defaults) */
  can_update_init(&can_state, &can_hal, can_cmd_id);
  can_initialized = (can_polled_init(CAN_BITRATE_BPS, can_cmd_id) == CAN_POLLED_OK);
#endif

  while (1)
  {
    tud_task(); // tinyusb device task
    led_blinking_task();
#ifdef CAN_LOADER
    if (can_initialized) {
      can_service_once();
    }
#endif
  }
}

/*------------- MAIN -------------*/
int main(void)
{
  board_init();

#if defined(CAN_LOADER) && defined(CAN_DIAG)
  /* DIAG4 staircase entry (replaces DIAG3's E0 loop): runs once per power
   * cycle, from the exact spot DIAG3 confirmed bare CAN TX dead. The only
   * new instruction before probe 0 is the VBTBKR[8] flag read here — a
   * VBATT read cannot be the E0 killer, since E0 failed with no VBTBKR
   * access at all. The staircase boot rides the real `bootloader:` preamble
   * with probes interleaved and ends in one software reset, so the
   * double-tap / CAN-magic / app decisions below always start from clean
   * peripherals; a pending magic survives that boot untouched and is
   * honored one boot later. See the staircase block above run_bootloader()
   * for the design and the no-boot-loop argument. */
  if (CAN_DIAG_STAIR_DONE_DATA != CAN_DIAG_STAIR_DONE_MAGIC) {
    can_diag_stair_set_done();  /* BEFORE the run: a crash cannot re-enter */
    can_diag_stair_active = true;
    goto bootloader;
  }
#endif

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
  /* Validate -> decide (rules/bootloader.md): the CRC metadata block at
   * METADATA_ADDR is authoritative. Metadata absent/corrupt falls back to
   * the stock SP-word check (documented deviation, for images flashed by
   * tools that write no metadata, e.g. SWD). Metadata intact but the image
   * failing any check fails closed into the bootloader. */
  int app_valid;
  switch (image_validate((const uint8_t *) 0)) {
    case IMAGE_VALID:
      app_valid = 1;
      break;
    case IMAGE_ERR_MAGIC:
    case IMAGE_ERR_HEADER_CRC:
      app_valid = (((*(uint32_t *) SKETCH_FLASH_OFFSET) & 0xFF000000) == 0x20000000);
      break;
    default:
      app_valid = 0;
      break;
  }
#else
  int app_valid = (((*(uint32_t *) SKETCH_FLASH_OFFSET) & 0xFF000000) == 0x20000000);
#endif

  if (app_valid) {
      boot5(SKETCH_FLASH_OFFSET);
  }

bootloader:

#if defined(CAN_LOADER) && defined(CAN_DIAG)
  can_diag_stair_probe(0);  /* bare: board_init only (DIAG3's E0 conditions) */
#endif

  R_IOPORT_Open(&port_ctrl, &pin_cfg);

#if defined(CAN_LOADER) && defined(CAN_DIAG)
  can_diag_stair_probe(1);  /* + IOPORT open */
#endif

  R_GPT_Open(&pwm_ctrl, &pwm_cfg);
  R_GPT_PeriodSet(&pwm_ctrl, PERIOD);
  R_GPT_DutyCycleSet(&pwm_ctrl, 0, pwm_channel);
  R_GPT_Start(&pwm_ctrl);

#if defined(CAN_LOADER) && defined(CAN_DIAG)
  can_diag_stair_probe(2);  /* + GPT open/period/duty/start */
#endif

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

#if defined(CAN_LOADER) && defined(CAN_DIAG)
  can_diag_stair_probe(3);  /* + FLASH_LP open/reset (data-flash gate) */
#endif

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

#ifndef ZEPHYR_SKETCH_FLASH_OFFSET
#define ZEPHYR_SKETCH_FLASH_OFFSET SKETCH_FLASH_OFFSET
#endif
static uint32_t _offset;

#ifdef CAN_LOADER
static uint32_t dfu_received_len;

/* Research §5.2 / risk R7: stock DFU uploads carry no CRC. After a completed
 * DFU download, compute CRC32 over the received image and write a fresh
 * metadata block, so DFU-uploaded sketches are CRC-validated on later boots.
 * Every early return leaves the boot decision on the stock SP-check fallback
 * — degraded validation, never a brick. */
static void dfu_write_image_metadata(void)
{
  /* An oversized sketch (> APP_SLOT_SIZE) already overwrote the metadata
   * block; leave its bytes alone (research §5.1: such images are simply
   * unvalidated). */
  if (dfu_received_len == 0 || dfu_received_len > APP_SLOT_SIZE) {
    return;
  }

  __disable_irq();
  int err = R_FLASH_LP_Erase(g_flash.p_ctrl, METADATA_ADDR, 1);
  __enable_irq();
  if (err != FSP_SUCCESS) {
    return;
  }

  image_meta_t meta = {
    .magic        = IMAGE_META_MAGIC,
    .image_size   = dfu_received_len,
    .image_crc32  = crc32_ieee((const uint8_t *) APP_BASE, dfu_received_len),
    .header_crc32 = 0,
  };
  meta.header_crc32 = crc32_ieee((const uint8_t *) &meta, IMAGE_META_CRC_COVERAGE);

  (void) can_flash_program(METADATA_ADDR, (const uint8_t *) &meta, sizeof(meta));
}
#endif

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

#ifdef CAN_LOADER
  if (block_num == 0) {
    dfu_received_len = 0;
  }
  dfu_received_len += length;
#endif

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

#ifdef CAN_LOADER
  dfu_write_image_metadata();
#endif

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

// Invoked when a DFU_DETACH request is received
void tud_dfu_detach_cb(void)
{
  //printf("Host detach, we should probably reboot\r\n");
  uint32_t start_ms = board_millis();
  while (board_millis() - start_ms < 100) {
    tud_task();
  }
  NVIC_SystemReset();
}

#endif

//--------------------------------------------------------------------+
// BLINKING TASK + Indicator pulse
//--------------------------------------------------------------------+

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

//--------------------------------------------------------------------+
// Double tap magic
//--------------------------------------------------------------------+
void set_double_tap_data(uint32_t data)
{
  R_SYSTEM->PRCR = (uint16_t) BSP_PRV_PRCR_PRC1_UNLOCK;
  BOOT_DOUBLE_TAP_DATA = data;
  R_SYSTEM->PRCR = (uint16_t) BSP_PRV_PRCR_LOCK;
}
