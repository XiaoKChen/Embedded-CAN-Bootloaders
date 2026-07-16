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
 * Data flash is readable here — R_FLASH_LP_Open() enabled DFLCTL.DFLEN before
 * either CAN service path runs. Absent/invalid/v1 block or an out-of-range ID
 * field leaves the compiled defaults (dfu_config_can_ids fills them in). A build
 * without DFU_CUSTOM_NAME has no config parser, so it keeps the defaults. */
static void can_load_ids(void)
{
#ifdef DFU_CUSTOM_NAME
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
 * flash, readable here — R_FLASH_LP_Open() enabled DFLCTL.DFLEN before either
 * CAN service path runs, as the descriptor path also relies on). Absent/invalid
 * block -> name_len = 0 and all 32 name bytes 0x00. A build without
 * DFU_CUSTOM_NAME has no config parser, so it reports name_len = 0. */
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
  if (dfu_config_parse((const uint8_t *) DFU_CONFIG_ADDR,
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

/* CAN update mode (entered via the VBTBKR[4] magic): CAN-only loop; the only
 * exits are the GO reset above or an external reset. */
static void run_can_update_mode(void)
{
  R_FLASH_LP_Open(g_flash.p_ctrl, g_flash.p_cfg);
  R_FLASH_LP_Reset(g_flash.p_ctrl);

  can_load_ids();  /* per-device IDs from the v2 config block (or defaults) */
  can_update_init(&can_state, &can_hal, can_cmd_id);
  if (can_polled_init(CAN_BITRATE_BPS, can_cmd_id) != CAN_POLLED_OK) {
    NVIC_SystemReset();  /* CAN unusable: reset into the normal boot path */
  }
  can_initialized = true;
  while (1) {
    can_service_once();
  }
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

/* ***
 * UNO R4 MINIMA AND C33 BOOTLOADER ** ONLY DFU **
 * *** */
__WEAK void run_bootloader() {
  // init device stack on configured roothub port
  tud_init(BOARD_TUD_RHPORT);
  if (board_init_after_tusb) {
    board_init_after_tusb();
  }

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

  R_IOPORT_Open(&port_ctrl, &pin_cfg);

  R_GPT_Open(&pwm_ctrl, &pwm_cfg);
  R_GPT_PeriodSet(&pwm_ctrl, PERIOD);
  R_GPT_DutyCycleSet(&pwm_ctrl, 0, pwm_channel);
  R_GPT_Start(&pwm_ctrl);

#if BSP_FEATURE_FLASH_HP_VERSION
  R_FLASH_HP_Open(g_flash.p_ctrl, g_flash.p_cfg);
  R_FLASH_HP_Reset(g_flash.p_ctrl);
  R_FLASH_HP_StartUpAreaSelect(g_flash.p_ctrl, FLASH_STARTUP_AREA_BLOCK0, true);
#endif

#if BSP_FEATURE_FLASH_LP_VERSION
  R_FLASH_LP_Open(g_flash.p_ctrl, g_flash.p_cfg);
  R_FLASH_LP_Reset(g_flash.p_ctrl);
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
