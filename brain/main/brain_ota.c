/* =============================================================================
 * brain_ota.c — Brain self-update over Ethernet: entry point and self-test
 * =============================================================================
 * Bench firmware for the brain (ESP32-S3-WROOM-1U-N16R8, W5500 on SPI2):
 *   - drives GPIO3 LOW first, so the node is not held in reset;
 *   - logs its version, running slot and OTA state;
 *   - brings up Ethernet (DHCP) and the HTTP server (GET /, POST /ota);
 *   - when it boots as a fresh OTA image (PENDING_VERIFY), its self-test is
 *     "Ethernet got an IP within SELF_TEST_IP_TIMEOUT_MS": it then marks
 *     itself valid, otherwise it rolls back and reboots. A
 *     CONFIG_BRAIN_OTA_SKIP_MARK_VALID build stays PENDING_VERIFY, so the
 *     next reset rolls it back (docs/testing/bench/brain-ota.md).
 * ============================================================================= */

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

#include "eth_w5500.h"
#include "http_ota_server.h"
#include "node_reset.h"
#include "node_flash.h"
#include "ota_update.h"


/* -----------------------------------------------------------------------------
 * Constants
 * ----------------------------------------------------------------------------- */
#define SELF_TEST_IP_TIMEOUT_MS  30000u
#define MS_PER_S                 1000u

static const char *TAG = "brain_ota";


/* -----------------------------------------------------------------------------
 * Private helpers
 * ----------------------------------------------------------------------------- */
static void log_identity(void)
{
    const esp_app_desc_t  *desc    = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();

    ESP_LOGI(TAG, "%s version %s (ESP-IDF %s), running from %s (0x%" PRIx32 "), OTA state %s",
             desc->project_name, desc->version, desc->idf_ver,
             (running != NULL) ? running->label : "?",
             (running != NULL) ? running->address : 0u,
             ota_update_running_state_name());
}

static bool start_network(void)
{
    esp_err_t err = eth_w5500_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet start failed: %s", esp_err_to_name(err));
        return false;
    }
    err = http_ota_server_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

static bool running_image_pending(void)
{
    esp_ota_img_states_t state;
    return esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
           state == ESP_OTA_IMG_PENDING_VERIFY;
}

static void confirm_image(bool network_up)
{
    if (!running_image_pending()) {
        return;
    }
    ESP_LOGI(TAG, "self-test: new image, waiting up to %u s for an IP",
             SELF_TEST_IP_TIMEOUT_MS / MS_PER_S);
    const bool passed = network_up && eth_w5500_wait_for_ip(pdMS_TO_TICKS(SELF_TEST_IP_TIMEOUT_MS));
    if (!passed) {
        ESP_LOGE(TAG, "self-test failed (no network within %u s); rolling back and rebooting",
                 SELF_TEST_IP_TIMEOUT_MS / MS_PER_S);
        const esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();  /* returns only on failure */
        ESP_LOGE(TAG, "rollback failed: %s; staying on this image", esp_err_to_name(err));
        return;
    }
#ifdef CONFIG_BRAIN_OTA_SKIP_MARK_VALID
    ESP_LOGW(TAG, "self-test passed, but this SKIP_MARK_VALID build leaves the image "
                  "unconfirmed; the next reset rolls back to the previous slot");
#else
    const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "marking the image valid failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "self-test passed; image marked VALID (rollback cancelled)");
#endif
}


/* -----------------------------------------------------------------------------
 * Entry
 * ----------------------------------------------------------------------------- */
void app_main(void)
{
    ESP_ERROR_CHECK(node_reset_release());  /* first: GPIO3 floats until then */
    ESP_LOGI(TAG, "GPIO%d (node reset) driven LOW", CONFIG_NODE_RESET_GPIO);
    log_identity();
    const esp_err_t can_err = node_flash_init();
    if (can_err != ESP_OK) {
        ESP_LOGE(TAG, "node CAN host unavailable: %s; own OTA remains available", esp_err_to_name(can_err));
    }
    confirm_image(start_network());
    http_ota_server_resume_packages();
    /* Returning ends only this task; Ethernet and the HTTP server keep running. */
}
