/* =============================================================================
 * bench_app.c — Minimal OTA app for bench-testing the ESP32-S3 CAN updater
 * =============================================================================
 * Delivered over CAN by tools/can-flash (docs/bench-test.md). It:
 *   - logs its version, running partition and OTA state at boot;
 *   - after a trivial self-test confirms itself with
 *     esp_ota_mark_app_valid_cancel_rollback(), unless
 *     CONFIG_BENCH_APP_SKIP_MARK_VALID leaves it PENDING_VERIFY so the next
 *     reset rolls back into the updater;
 *   - on the console, `u` + Enter calls can_updater_client_enter_updater().
 * It sends no CAN traffic of its own.
 * ============================================================================= */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "can_updater_client.h"
#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"


/* -----------------------------------------------------------------------------
 * Constants
 * ----------------------------------------------------------------------------- */
#define ENTER_UPDATER_COMMAND  "u"
#define CONSOLE_LINE_MAX       16u
#define CONSOLE_POLL_MS        50u

static const char *TAG = "bench_app";


/* -----------------------------------------------------------------------------
 * Private helpers
 * ----------------------------------------------------------------------------- */
static const char *ota_state_name(esp_ota_img_states_t state)
{
    switch (state) {
    case ESP_OTA_IMG_NEW:            return "NEW";
    case ESP_OTA_IMG_PENDING_VERIFY: return "PENDING_VERIFY";
    case ESP_OTA_IMG_VALID:          return "VALID";
    case ESP_OTA_IMG_INVALID:        return "INVALID";
    case ESP_OTA_IMG_ABORTED:        return "ABORTED";
    case ESP_OTA_IMG_UNDEFINED:      return "UNDEFINED";
    default:                         return "?";
    }
}

static void log_identity(void)
{
    const esp_app_desc_t  *desc    = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();

    esp_ota_img_states_t state;
    const char *state_text = "n/a (not an OTA slot)";
    if (running != NULL && esp_ota_get_state_partition(running, &state) == ESP_OK) {
        state_text = ota_state_name(state);
    }

    ESP_LOGI(TAG, "%s version %s (ESP-IDF %s), running from %s, OTA state %s",
             desc->project_name, desc->version, desc->idf_ver,
             (running != NULL) ? running->label : "?", state_text);
}

static bool self_test_passed(void)
{
    return (esp_ota_get_running_partition() != NULL) &&
           heap_caps_check_integrity_all(true);
}

static void confirm_image(void)
{
    if (!self_test_passed()) {
        ESP_LOGE(TAG, "self-test failed: image left unconfirmed; "
                      "the next reset rolls back to the CAN updater");
        return;
    }
#ifdef CONFIG_BENCH_APP_SKIP_MARK_VALID
    ESP_LOGW(TAG, "self-test passed, but this SKIP_MARK_VALID build leaves the image "
                  "unconfirmed; the next reset rolls back to the CAN updater");
#else
    const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "marking the image valid failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "self-test passed; image marked valid (rollback cancelled)");
#endif
}

static void run_command(const char *line)
{
    if (strcmp(line, ENTER_UPDATER_COMMAND) == 0) {
        ESP_LOGI(TAG, "entering the CAN updater");
        const esp_err_t err = can_updater_client_enter_updater();  /* restarts on success */
        ESP_LOGE(TAG, "cannot enter the CAN updater: %s", esp_err_to_name(err));
    } else if (line[0] != '\0') {
        ESP_LOGW(TAG, "unknown command '%s'; type 'u' + Enter for the CAN updater", line);
    }
}

/* Console reads are non-blocking without a driver (UART and USB-Serial-JTAG
 * VFS alike): EOF only means "no input yet", so clear it and poll again. */
static void serve_console_forever(void)
{
    char   line[CONSOLE_LINE_MAX];
    size_t len = 0u;

    for (;;) {
        const int c = fgetc(stdin);
        if (c == EOF) {
            clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(CONSOLE_POLL_MS));
            continue;
        }
        if (c != '\r' && c != '\n') {
            if (len < sizeof(line) - 1u) {
                line[len++] = (char)c;
            }
            continue;
        }
        line[len] = '\0';
        len = 0u;
        run_command(line);
    }
}


/* -----------------------------------------------------------------------------
 * Entry
 * ----------------------------------------------------------------------------- */
void app_main(void)
{
    log_identity();
    confirm_image();
    ESP_LOGI(TAG, "type 'u' + Enter to enter the CAN updater");
    serve_console_forever();
}
