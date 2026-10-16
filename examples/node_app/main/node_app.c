/* =============================================================================
 * node_app.c — Reference node application (Phase 6, issue #11)
 * =============================================================================
 * Start real node firmware from this file. Boot sequence, in order:
 *   1. can_updater_client_start_health()  Phase 3 recovery: marks deliberate
 *      restarts clean and clears the crash counters after 10 s of uptime.
 *   2. The node's CAN update IDs: `cancfg` when provisioned, else 0x79E/0x79F
 *      (the same IDs the updater answers on, so one ID pair addresses the node
 *      whether it runs this app or sits in the updater).
 *   3. TWAI at CONFIG_NODE_APP_CAN_BITRATE_BPS on the node pins, plus the CAN
 *      service task (can_service.c) answering APP_INFO and ENTER_UPDATER.
 *   4. Self-test, then esp_ota_mark_app_valid_cancel_rollback(). A failed
 *      self-test leaves a freshly CAN-flashed image PENDING_VERIFY, so the
 *      next reset rolls back (to the previous app or the updater).
 *   5. app_run(): YOUR APPLICATION HERE.
 *
 * Updating this node: the brain (POST /package or /node/flash) or
 * tools/can-flash sends ENTER_UPDATER; the service ACKs it and restarts into
 * the updater, which then takes the image. No GPIO3 pulse is needed.
 * ============================================================================= */

#include <stdbool.h>

#include "can_service.h"
#include "can_updater_client.h"
#include "dfu_config.h"
#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


/* -----------------------------------------------------------------------------
 * Constants
 * ----------------------------------------------------------------------------- */
#define APP_LOOP_PERIOD_MS  1000u

static const char *TAG = "node_app";


/* -----------------------------------------------------------------------------
 * Private helpers
 * ----------------------------------------------------------------------------- */
static void log_identity(void)
{
    const esp_app_desc_t  *desc    = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();

    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    const bool has_state = (running != NULL) && (esp_ota_get_state_partition(running, &state) == ESP_OK);
    ESP_LOGI(TAG, "%s version %s (ESP-IDF %s), running from %s%s",
             desc->project_name, desc->version, desc->idf_ver,
             (running != NULL) ? running->label : "?",
             (has_state && state == ESP_OTA_IMG_PENDING_VERIFY) ? ", PENDING_VERIFY" : "");
}

/* Extend with your own checks (sensors answer, config valid, ...): anything
 * that must work before this image may replace the previous one for good. */
static bool self_test_passed(const can_service_ids_t *ids, esp_err_t can_status)
{
    bool passed = true;
    if (can_status != ESP_OK) {
        ESP_LOGE(TAG, "self-test: CAN service not running (%s)", esp_err_to_name(can_status));
        passed = false;
    }
    if (ids->cmd_id > DFU_CONFIG_CAN_ID_MAX || ids->reply_id > DFU_CONFIG_CAN_ID_MAX ||
        ids->cmd_id == ids->reply_id) {
        ESP_LOGE(TAG, "self-test: unusable CAN IDs 0x%03X / 0x%03X", ids->cmd_id, ids->reply_id);
        passed = false;
    }
    return passed;
}

static void confirm_image(bool self_test_ok)
{
    if (!self_test_ok) {
        ESP_LOGE(TAG, "self-test FAILED: image NOT marked valid; if it is still pending, "
                      "the next reset rolls back");
        return;
    }
    const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "marking the image valid failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "self-test passed; image marked valid (rollback cancelled)");
}

/* =============================================================================
 * YOUR APPLICATION HERE
 * =============================================================================
 * Replace this loop with the node's real work. The CAN service keeps running
 * in its own task. Rules that keep the node updatable:
 *   - leave the update cmd/reply IDs to can_service (use other IDs for data);
 *   - restart only with esp_restart() (clean for Phase 3 recovery);
 *   - do not block app_main before confirm_image() has run.
 * ============================================================================= */
static void app_run(void)
{
    ESP_LOGI(TAG, "application running (placeholder: replace app_run() in node_app.c)");
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(APP_LOOP_PERIOD_MS));
    }
}


/* -----------------------------------------------------------------------------
 * Entry
 * ----------------------------------------------------------------------------- */
void app_main(void)
{
    ESP_ERROR_CHECK(can_updater_client_start_health());
    log_identity();

    can_service_ids_t ids;
    can_service_load_ids(&ids);
    const esp_err_t can_status = can_service_start(&ids);

    confirm_image(self_test_passed(&ids, can_status));
    app_run();
}
