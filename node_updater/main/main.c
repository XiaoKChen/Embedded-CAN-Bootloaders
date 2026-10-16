/* =============================================================================
 * main.c — ESP32-S3 CAN (TWAI) updater: the `factory` app
 * =============================================================================
 * The node's ESP-IDF 2nd-stage bootloader runs this app when no OTA app is
 * selected: on a fresh board, after an app called can_updater_client (which
 * points the boot partition here), or when every OTA app failed validation /
 * was rolled back. Recovery counters also select factory without changing
 * otadata (no GPIO recovery: GPIO4 is TWAI TX). In all those cases the user wants
 * the updater, so it always parks serving CAN Update Protocol v1 — the only
 * exits are a completed update (GO) or a re-flash over USB/UART.
 *
 * The protocol engine is the shared pure-logic core (src/boot/can_update.c);
 * this file supplies its slot HAL over the ESP-IDF OTA API:
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
 * The factory partition (this app) is never written by anything here.
 * ============================================================================= */

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


/* -----------------------------------------------------------------------------
 * Constants (partition names mirror partitions.csv, the source of truth)
 * ----------------------------------------------------------------------------- */
#define CAN_CFG_PARTITION_LABEL    "cancfg"
#define CAN_CFG_PARTITION_SUBTYPE  ((esp_partition_subtype_t)0x40)

#define RX_POLL_TIMEOUT_MS         1000u  /* bounds the watchdog-feed period */
#define LINK_RETRY_DELAY_MS        1000u  /* TWAI bring-up retry interval    */

static const char *TAG = "can_updater";


/* -----------------------------------------------------------------------------
 * File-local state
 * ----------------------------------------------------------------------------- */
static const esp_partition_t *s_target;    /* OTA slot this updater writes    */
static esp_ota_handle_t       s_ota;
static bool                   s_ota_open;
static uint32_t               s_ota_written;  /* bytes accepted by s_ota       */
static uint8_t                s_readback[CAN_UPDATE_WINDOW_SIZE];

static uint8_t            s_cfg_block[DFU_CONFIG_SIZE];
static uint16_t           s_cmd_id;
static uint16_t           s_reply_id;
static can_update_hal_t   s_hal;
static can_update_state_t s_engine;


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

    dfu_config_can_ids(s_cfg_block, &s_cmd_id, &s_reply_id);
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

static void bring_up_link(void)
{
    const can_link_config_t link_cfg = {
        .tx_gpio     = CONFIG_CAN_UPDATER_TWAI_TX_GPIO,
        .rx_gpio     = CONFIG_CAN_UPDATER_TWAI_RX_GPIO,
        .bitrate_bps = CONFIG_CAN_UPDATER_BITRATE_BPS,
        .accept_id   = s_cmd_id,
    };
    while (can_link_init(&link_cfg) != CAN_LINK_STATUS_OK) {
        ESP_LOGE(TAG, "TWAI init failed (TX GPIO %d, RX GPIO %d); retrying",
                 CONFIG_CAN_UPDATER_TWAI_TX_GPIO, CONFIG_CAN_UPDATER_TWAI_RX_GPIO);
        (void)esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(LINK_RETRY_DELAY_MS));
    }
}

static void serve_can_forever(void)
{
    for (;;) {
        (void)esp_task_wdt_reset();

        uint32_t id;
        uint8_t  dlc;
        uint8_t  data[8];
        if (can_link_receive(&id, &dlc, data, RX_POLL_TIMEOUT_MS) != CAN_LINK_STATUS_OK) {
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
    ESP_ERROR_CHECK(can_updater_client_start_health());
    load_config_block();

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
             s_cmd_id, s_reply_id, (unsigned)CONFIG_CAN_UPDATER_BITRATE_BPS,
             (s_target != NULL) ? s_target->label : "none",
             (unsigned long)s_hal.slot_size);

    /* A hung updater must reset back into itself (CONFIG_ESP_TASK_WDT_PANIC).
     * A subscription failure is logged, not fatal: parking beats rebooting. */
    if (esp_task_wdt_add(NULL) != ESP_OK) {
        ESP_LOGW(TAG, "task watchdog subscription failed");
    }

    bring_up_link();
    serve_can_forever();
}
