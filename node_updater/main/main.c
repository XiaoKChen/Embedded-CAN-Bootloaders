/**
 * @file    main.c
 * @brief   Board-profiled ESP32-S3 CAN updater: the shared `factory` app.
 *
 * The node's ESP-IDF 2nd-stage bootloader runs this app when no OTA app is selected: on a fresh
 * board, after an app called can_updater_client (which points the boot partition here), or when
 * every OTA app failed validation / was rolled back. Recovery counters also select factory
 * without changing otadata. Provisioned GPIO holds and I2C safe writes precede services and the
 * writes repeat every second while parked; cancfg also selects TWAI wiring and USB CDC identity,
 * with a 1200-baud ROM-download exit.
 *
 * Owns:         The slot HAL of the shared pure-logic engine (src/boot/can_update.c) over the
 *               ESP-IDF OTA API, the INFO reply, CAN IDs and board profile from `cancfg`, the
 *               updater's GPIO-driver HAL for i2c_bitbang and the 1 s safe-write cadence.
 * Not here:     The protocol itself (can_update.h); the recovery-counter decision
 *               (node_updater/bootloader_components/main/boot.c); the I2C sequence
 *               (i2c_bitbang.h).
 * Invariants:   In all those cases the user wants the updater, so it always parks serving CAN
 *               Update Protocol v1 — the only exits are a completed update (GO) or a re-flash over
 *               USB/UART. The factory partition (this app) is never written by anything here.
 *               Safety holds always win: default TWAI fallback is skipped if either default
 *               pin is held LOW; no failed-link recovery may reconfigure a held pin or drop the
 *               I2C safe writes. The writes repeat every SAFE_WRITE_PERIOD_MS (the CAN wait is
 *               cut to the next one), each run bounded by I2C_BB_WORST_CASE_US, well inside the
 *               TWAI RX queue's buffering; their failure never stops the updater.
 * Depends on:   can_link.h, can_update.h, dfu_config.h, board_profile.h, i2c_bitbang.h,
 *               cancfg.h, can_updater_client.h, usb_cdc.h.
 *
 * @see     can_update.h, docs/testing/bench/node-updater.md, docs/concepts/recovery.md
 */

/*
 * Slot HAL over the ESP-IDF OTA API:
 *   ERASE  -> esp_ota_begin(OTA_WITH_SEQUENTIAL_WRITES) on the inactive slot
 *   DATA   -> esp_ota_write + esp_partition_read read-back verify
 *   VERIFY -> engine CRC over esp_partition_read, then esp_ota_end
 *             (ESP image format, chip ID and SHA-256 validation)
 *   GO     -> verified recovery-counter clear, esp_ota_set_boot_partition, restart
 *
 * Power-loss table (the otadata write in GO is the only boot-selection change
 * and ESP-IDF makes it atomic across its two sectors):
 *   during ERASE/DATA/VERIFY .. target slot torn or unselected; otadata
 *                                unchanged -> reboots into this updater
 *   after VERIFY, before GO ... image complete but unselected -> updater
 *   during GO ................. otadata keeps its previous valid copy ->
 *                                updater, or the new app once it landed
 *   after GO .................. new app boots PENDING_VERIFY; if it never
 *                                calls esp_ota_mark_app_valid_cancel_rollback
 *                                the bootloader rolls back -> updater
 * For counter-forced entry, the prior OTA selection remains the fallback
 * after the counters clear (10 s stable or verified GO); interrupted updates
 * can return to that app and require recovery again. Pending rollback may
 * return to a prior valid OTA image instead of factory. A GO clear interrupted
 * before otadata changes never selects the new image. Clear failure rejects GO.
 */

#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "can_updater_client.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "can_link.h"
#include "can_update.h"
#include "dfu_config.h"
#include "board_profile.h"
#include "i2c_bitbang.h"
#include "../cancfg.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "usb_cdc.h"


/* -----------------------------------------------------------------------------
 * Service timing (partition identity lives in cancfg.h)
 * ----------------------------------------------------------------------------- */
#define RX_POLL_TIMEOUT_MS         1000u  /* bounds the watchdog-feed period */
#define RECOVERY_POLL_DELAY_MS     1000u  /* feed watchdog while USB-only   */
#define LINK_START_ATTEMPTS        2u     /* profile, then at most defaults */
#define SAFE_WRITE_PERIOD_MS       1000u  /* re-arm a re-powered output chip well before
                                           * the EMC2305's 4 s power-up watchdog       */

static const char *TAG = "can_updater";


/* -----------------------------------------------------------------------------
 * File-local state
 * ----------------------------------------------------------------------------- */
static const esp_partition_t *s_target;    /* OTA slot this updater writes    */
static esp_ota_handle_t       s_ota;
static bool                   s_ota_open;
static uint32_t               s_ota_written;  /* bytes accepted by s_ota       */
static uint8_t                s_readback[CAN_UPDATE_WINDOW_SIZE];

static uint8_t            s_cfg_block[BOARD_PROFILE_CANCFG_OFFSET + BOARD_PROFILE_SIZE];
static board_profile_t    s_profile;
static bool               s_profile_from_record;
static uint16_t           s_cmd_id;
static uint16_t           s_reply_id;
static can_update_hal_t   s_hal;
static can_update_state_t s_engine;
static bool               s_i2c_ready;      /* I2C pins configured for safe writes */
static TickType_t         s_last_safe_write;


/* -----------------------------------------------------------------------------
 * Slot HAL over the ESP-IDF OTA API
 * ----------------------------------------------------------------------------- */
static bool slot_erase(void)
{
    if (s_target == NULL) {
        return false;
    }
    if (s_ota_open) {
        (void)esp_ota_abort(s_ota);  /* abandon the earlier session */
        s_ota_open = false;
    }
    s_ota_written = 0u;

    /* Sequential mode erases each 4 KB sector as the write reaches it, so
     * ERASE answers at once instead of wiping ~1.8 MB up front (seconds; the
     * host waits 15 s at most) and each window ACK pays one sector erase. */
    if (esp_ota_begin(s_target, OTA_WITH_SEQUENTIAL_WRITES, &s_ota) != ESP_OK) {
        return false;
    }
    s_ota_open = true;
    return true;
}

static bool slot_program(uint32_t offset, const uint8_t *data, uint32_t len)
{
    /* A sequential OTA handle cannot rewind: a retry of a window whose write
     * already advanced the handle cannot be honoured, so it fails closed and
     * the host restarts the session with HELLO. */
    if (!s_ota_open || offset != s_ota_written) {
        return false;
    }
    if (esp_ota_write(s_ota, data, len) != ESP_OK) {
        return false;  /* handle not advanced: the window retry is legal */
    }
    s_ota_written += len;

    if (esp_partition_read(s_target, offset, s_readback, len) != ESP_OK) {
        return false;
    }
    return memcmp(s_readback, data, len) == 0;
}

static bool slot_read(uint32_t offset, uint8_t *buf, uint32_t len)
{
    return (s_target != NULL) && (esp_partition_read(s_target, offset, buf, len) == ESP_OK);
}

static bool slot_commit(void)
{
    if (!s_ota_open) {
        return false;
    }
    s_ota_open = false;  /* esp_ota_end releases the handle on every path */
    const esp_err_t err = esp_ota_end(s_ota);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "image rejected: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

static bool slot_activate(void)
{
    /* The engine only calls this after successful VERIFY. Clear before OTA
     * selection so a small image can boot even inside the 10 s trip window. */
    const esp_err_t clear_err = can_updater_client_prepare_verified_boot();
    if (clear_err != ESP_OK) {
        ESP_LOGE(TAG, "recovery clear failed; GO refused: %s", esp_err_to_name(clear_err));
        return false;
    }
    const esp_err_t err = esp_ota_set_boot_partition(s_target);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set boot partition failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}


/* -----------------------------------------------------------------------------
 * Per-device configuration
 * ----------------------------------------------------------------------------- */

/* Read the provisioning block from the raw `cancfg` partition. Missing
 * partition or failed read leaves the buffer erased (0xFF) — exactly what an
 * unprovisioned board holds — so dfu_config_* fall back to the defaults
 * (0x79E / 0x79F, no INFO name). Never faults. */
static void load_config_block(void)
{
    memset(s_cfg_block, 0xFF, sizeof(s_cfg_block));

    const esp_partition_t *cfg = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, CAN_CFG_PARTITION_SUBTYPE, CAN_CFG_PARTITION_LABEL);
    if (cfg == NULL ||
        esp_partition_read(cfg, 0u, s_cfg_block, sizeof(s_cfg_block)) != ESP_OK) {
        memset(s_cfg_block, 0xFF, sizeof(s_cfg_block));
        ESP_LOGW(TAG, "config partition unreadable; using default CAN IDs");
    }

    s_profile_from_record = board_profile_parse(
        s_cfg_block + BOARD_PROFILE_CANCFG_OFFSET, BOARD_PROFILE_SIZE, &s_profile) == BOARD_PROFILE_OK;
    const uint64_t held = s_profile.hold_low_mask | s_profile.hold_high_mask;
    /* Set the output latch before enabling output: never pulse a held pin to the wrong level. */
    for (uint32_t pin = 0u; pin <= BOARD_PROFILE_MAX_GPIO; ++pin) {
        if ((held & (1ull << pin)) != 0u) {
            ESP_ERROR_CHECK(gpio_set_level((gpio_num_t)pin,
                                           (s_profile.hold_high_mask & (1ull << pin)) != 0u));
        }
    }
    if (held != 0u) {
        const gpio_config_t holds = {
            .pin_bit_mask = held,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&holds));
    }
    dfu_config_can_ids(s_cfg_block, &s_cmd_id, &s_reply_id);
}


/* -----------------------------------------------------------------------------
 * I2C safe writes (pins from the board profile; sequence in i2c_bitbang.c)
 * ----------------------------------------------------------------------------- */
static void i2c_sda_release(void *ctx, bool release)
{
    (void)ctx;
    (void)gpio_set_level((gpio_num_t)s_profile.i2c_sda_gpio, release);
}

static void i2c_scl_release(void *ctx, bool release)
{
    (void)ctx;
    (void)gpio_set_level((gpio_num_t)s_profile.i2c_scl_gpio, release);
}

static bool i2c_sda_read(void *ctx)
{
    (void)ctx;
    return gpio_get_level((gpio_num_t)s_profile.i2c_sda_gpio) != 0;
}

static bool i2c_scl_read(void *ctx)
{
    (void)ctx;
    return gpio_get_level((gpio_num_t)s_profile.i2c_scl_gpio) != 0;
}

static void i2c_delay_us(void *ctx, uint32_t us)
{
    (void)ctx;
    esp_rom_delay_us(us);
}

static const i2c_bb_hal_t s_i2c_hal = {
    NULL, i2c_sda_release, i2c_scl_release, i2c_sda_read, i2c_scl_read, i2c_delay_us,
};

/* Open-drain with pull-ups, released before the driver is enabled; the updater keeps the pins
 * for its whole life (the bootloader released them, the app owns them once it runs). */
static void start_safe_writes(void)
{
    if (s_profile.i2c_sda_gpio == BOARD_PROFILE_I2C_NONE) {
        return;
    }
    const gpio_config_t od = {
        .pin_bit_mask = (1ull << s_profile.i2c_sda_gpio) | (1ull << s_profile.i2c_scl_gpio),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    (void)gpio_set_level((gpio_num_t)s_profile.i2c_sda_gpio, 1);
    (void)gpio_set_level((gpio_num_t)s_profile.i2c_scl_gpio, 1);
    const esp_err_t err = gpio_config(&od);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C safe-write pins unavailable: %s", esp_err_to_name(err));
        return;
    }
    s_i2c_ready = true;
    const uint8_t acked = i2c_bb_apply_safe_writes(&s_i2c_hal, &s_profile);
    s_last_safe_write = xTaskGetTickCount();
    ESP_LOGI(TAG, "I2C safe writes 0x%02X on SDA %u SCL %u: %u/%u ACKed; repeating every %u ms",
             (unsigned)s_profile.i2c_addr7, (unsigned)s_profile.i2c_sda_gpio,
             (unsigned)s_profile.i2c_scl_gpio, (unsigned)acked,
             (unsigned)s_profile.i2c_write_count, (unsigned)SAFE_WRITE_PERIOD_MS);
}

/* Repeat the writes once SAFE_WRITE_PERIOD_MS has passed; return how long the caller may block
 * (at most @p idle_ms) before calling again. */
static uint32_t service_safe_writes(uint32_t idle_ms)
{
    if (!s_i2c_ready) {
        return idle_ms;
    }
    const TickType_t period = pdMS_TO_TICKS(SAFE_WRITE_PERIOD_MS);
    TickType_t elapsed = xTaskGetTickCount() - s_last_safe_write;
    if (elapsed >= period) {
        (void)i2c_bb_apply_safe_writes(&s_i2c_hal, &s_profile);
        s_last_safe_write = xTaskGetTickCount();
        elapsed = 0u;
    }
    const uint32_t until_next_ms = (uint32_t)pdTICKS_TO_MS(period - elapsed);
    return (until_next_ms < idle_ms) ? until_next_ms : idle_ms;
}


/* -----------------------------------------------------------------------------
 * CAN service
 * ----------------------------------------------------------------------------- */

/* INFO (host CMD 0x07), byte-identical to the RA4M1 bootloader: header frame
 * {0x79, 0x07, name_len, protocol_version, 0xFF x4}, then the 32-byte product
 * name field in four frames, 0x00-padded past name_len. Name = the config
 * block's product string; absent/invalid block -> name_len 0, all zeros.
 * Answered here because the engine emits one reply per frame. */
static void serve_info(void)
{
    uint8_t name[DFU_CONFIG_STR_MAX];
    uint8_t name_len = 0u;
    memset(name, 0x00, sizeof(name));

    const char *mfr;
    const char *prod;
    uint8_t mfr_len;
    uint8_t prod_len;
    if (dfu_config_parse(s_cfg_block, &mfr, &mfr_len, &prod, &prod_len) == DFU_CFG_OK) {
        name_len = prod_len;  /* dfu_config_parse guarantees <= 32 */
        memcpy(name, prod, prod_len);
    }

    uint8_t header[8];
    header[0] = CAN_UPDATE_REPLY_MARKER;
    header[1] = CAN_UPDATE_CMD_INFO;
    header[2] = name_len;
    header[3] = CAN_UPDATE_PROTOCOL_VERSION;
    memset(&header[4], CAN_UPDATE_FILL_BYTE, 4u);
    (void)can_link_send(s_reply_id, header);  /* host retries INFO on loss */

    for (uint32_t off = 0u; off < DFU_CONFIG_STR_MAX; off += 8u) {
        (void)can_link_send(s_reply_id, &name[off]);
    }
}

static bool bring_up_link(void)
{
    const uint64_t held_low_mask = s_profile.hold_low_mask;
    for (uint32_t attempt = 0u; attempt < LINK_START_ATTEMPTS; ++attempt) {
        ESP_LOGI(TAG, "TWAI profile %s: TX %u RX %u, %lu bit/s; safety holds 0x%llX",
                 s_profile_from_record ? "record" : "defaults",
                 s_profile.twai_tx_gpio, s_profile.twai_rx_gpio,
                 (unsigned long)board_profile_bitrate_bps(s_profile.bitrate),
                 (unsigned long long)held_low_mask);
        const can_link_config_t link_cfg = {
            .tx_gpio = s_profile.twai_tx_gpio,
            .rx_gpio = s_profile.twai_rx_gpio,
            .bitrate_bps = board_profile_bitrate_bps(s_profile.bitrate),
            .accept_id = s_cmd_id,
        };
        if (can_link_init(&link_cfg) == CAN_LINK_STATUS_OK) {
            return true;
        }
        ESP_LOGE(TAG, "TWAI init failed (TX %u RX %u)", s_profile.twai_tx_gpio, s_profile.twai_rx_gpio);
        if (attempt == 0u) {
            if ((held_low_mask & ((1ull << BOARD_PROFILE_DEFAULT_TX_GPIO) |
                                  (1ull << BOARD_PROFILE_DEFAULT_RX_GPIO))) != 0u) {
                ESP_LOGE(TAG, "default TWAI retry skipped: default pins are held LOW");
                return false;
            }
            ESP_LOGW(TAG, "retrying once with defaults; safety holds remain asserted");
        }
        /* Only the TWAI fields fall back: the driven holds and the I2C safe writes stay. */
        board_profile_t defaults;
        board_profile_defaults(&defaults);
        s_profile.twai_tx_gpio = defaults.twai_tx_gpio;
        s_profile.twai_rx_gpio = defaults.twai_rx_gpio;
        s_profile.bitrate      = defaults.bitrate;
        s_profile_from_record  = false;
    }
    return false;
}

static void serve_can_forever(void)
{
    for (;;) {
        (void)esp_task_wdt_reset();
        const uint32_t wait_ms = service_safe_writes(RX_POLL_TIMEOUT_MS);

        uint32_t id;
        uint8_t  dlc;
        uint8_t  data[8];
        if (can_link_receive(&id, &dlc, data, wait_ms) != CAN_LINK_STATUS_OK) {
            continue;
        }

        /* INFO bypasses the engine and leaves the session state untouched. */
        if (id == s_cmd_id && data[0] == CAN_UPDATE_CMD_INFO) {
            serve_info();
            continue;
        }

        uint8_t reply[8];
        can_update_action_t action = CAN_UPDATE_ACTION_NONE;
        if (can_update_handle_frame(&s_engine, id, dlc, data, reply, &action)) {
            (void)can_link_send(s_reply_id, reply);  /* host retries on loss */
        }
        if (action == CAN_UPDATE_ACTION_RESET) {
            /* GO: the boot partition is already switched (slot_activate). */
            can_link_deinit();
            esp_restart();
        }
    }
}


/* -----------------------------------------------------------------------------
 * Entry
 * ----------------------------------------------------------------------------- */
void app_main(void)
{
    load_config_block();  /* Holds and safe writes come before health, USB or CAN services. */
    start_safe_writes();
    ESP_ERROR_CHECK(can_updater_client_start_health());
    const esp_err_t usb_err = updater_usb_cdc_init(s_cfg_block);
    if (usb_err != ESP_OK) {
        ESP_LOGE(TAG, "USB CDC unavailable: %s; CAN remains available", esp_err_to_name(usb_err));
    }

    /* The slot otadata does NOT select: ota_0 while the factory app is the
     * boot partition, otherwise the other one. Writing it never touches the
     * image the device currently boots. */
    s_target = esp_ota_get_next_update_partition(esp_ota_get_boot_partition());

    s_hal = (can_update_hal_t){
        .slot_size     = (s_target != NULL) ? (uint32_t)s_target->size : 0u,
        .slot_erase    = slot_erase,
        .slot_program  = slot_program,
        .slot_read     = slot_read,
        .slot_commit   = slot_commit,
        .slot_activate = slot_activate,
    };
    can_update_init(&s_engine, &s_hal, s_cmd_id);

    if (s_target == NULL) {
        /* Still serve HELLO/INFO (slot size 0 NAKs every SIZE) so the board
         * stays identifiable on the bus instead of reboot-looping. */
        ESP_LOGE(TAG, "no OTA partition in the partition table");
    }
    ESP_LOGI(TAG, "parked: cmd ID 0x%03X, reply ID 0x%03X, %u bit/s, slot %s (%lu bytes)",
             s_cmd_id, s_reply_id, (unsigned)board_profile_bitrate_bps(s_profile.bitrate),
             (s_target != NULL) ? s_target->label : "none",
             (unsigned long)s_hal.slot_size);

    /* A hung updater must reset back into itself (CONFIG_ESP_TASK_WDT_PANIC).
     * A subscription failure is logged, not fatal: parking beats rebooting. */
    if (esp_task_wdt_add(NULL) != ESP_OK) {
        ESP_LOGW(TAG, "task watchdog subscription failed");
    }

    if (!bring_up_link()) {
        /* Keep USB recovery alive, without hammering a failed TWAI driver. */
        for (;;) {
            (void)esp_task_wdt_reset();  /* best effort if subscription failed */
            vTaskDelay(pdMS_TO_TICKS(service_safe_writes(RECOVERY_POLL_DELAY_MS)));
        }
    }
    serve_can_forever();
}
