/* =============================================================================
 * ota_update.c — Transport-independent A/B self-update of the running app
 * =============================================================================
 * See ota_update.h for the public interface and the power-loss table.
 * ============================================================================= */

#include "ota_update.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"


/* -----------------------------------------------------------------------------
 * Constants
 * ----------------------------------------------------------------------------- */
#define PROGRESS_STEPS     10u   /* one log line per 10 % */
#define PERCENT_PER_STEP   (100u / PROGRESS_STEPS)

static const char *TAG = "ota_update";


/* -----------------------------------------------------------------------------
 * File-local state (one update at a time)
 * ----------------------------------------------------------------------------- */
static bool                   s_active;
static esp_ota_handle_t       s_handle;
static const esp_partition_t *s_target;
static size_t                 s_expected_bytes;
static size_t                 s_received_bytes;
static uint32_t               s_next_progress_step;


/* -----------------------------------------------------------------------------
 * Private helpers
 * ----------------------------------------------------------------------------- */
static void log_progress(void)
{
    /* Sizes are bounded by the slot (a few MB), so the product cannot wrap. */
    const uint32_t step = (uint32_t)((s_received_bytes * PROGRESS_STEPS) / s_expected_bytes);
    if (step < s_next_progress_step) {
        return;
    }
    s_next_progress_step = step + 1u;
    ESP_LOGI(TAG, "OTA progress: %3" PRIu32 " %% (%u / %u bytes)",
             step * PERCENT_PER_STEP, (unsigned)s_received_bytes, (unsigned)s_expected_bytes);
}


/* -----------------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------------- */
esp_err_t ota_update_begin(size_t image_size)
{
    if (s_active) {
        return ESP_ERR_INVALID_STATE;
    }
    if (image_size == 0u) {
        return ESP_ERR_INVALID_SIZE;
    }
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (target == NULL) {
        ESP_LOGE(TAG, "OTA rejected: no inactive OTA slot in the partition table");
        return ESP_ERR_NOT_FOUND;
    }
    if (image_size > target->size) {
        ESP_LOGE(TAG, "OTA rejected: %u-byte image does not fit %s (%" PRIu32 " bytes)",
                 (unsigned)image_size, target->label, target->size);
        return ESP_ERR_INVALID_SIZE;
    }

    const esp_err_t err = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &s_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA rejected: esp_ota_begin failed: %s", esp_err_to_name(err));
        return err;
    }

    s_active             = true;
    s_target             = target;
    s_expected_bytes     = image_size;
    s_received_bytes     = 0u;
    s_next_progress_step = 1u;
    ESP_LOGI(TAG, "OTA begin: %u bytes into %s (offset 0x%" PRIx32 ", %" PRIu32 " bytes)",
             (unsigned)image_size, target->label, target->address, target->size);
    return ESP_OK;
}

esp_err_t ota_update_write(const void *chunk, size_t len)
{
    if (!s_active) {
        return ESP_ERR_INVALID_STATE;
    }
    if (len == 0u) {
        return ESP_OK;
    }
    if (len > s_expected_bytes - s_received_bytes) {
        ESP_LOGE(TAG, "OTA write overruns the announced %u bytes", (unsigned)s_expected_bytes);
        ota_update_abort();
        return ESP_ERR_INVALID_SIZE;
    }

    const esp_err_t err = esp_ota_write(s_handle, chunk, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA write at offset %u failed: %s",
                 (unsigned)s_received_bytes, esp_err_to_name(err));
        ota_update_abort();
        return err;
    }
    s_received_bytes += len;
    log_progress();
    return ESP_OK;
}

esp_err_t ota_update_finish(void)
{
    if (!s_active) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_received_bytes != s_expected_bytes) {
        ESP_LOGE(TAG, "OTA image incomplete: %u of %u bytes",
                 (unsigned)s_received_bytes, (unsigned)s_expected_bytes);
        ota_update_abort();
        return ESP_ERR_INVALID_SIZE;
    }

    s_active = false;
    esp_err_t err = esp_ota_end(s_handle);  /* frees the handle whatever the result */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA result: image rejected (%s); boot partition unchanged",
                 esp_err_to_name(err));
        return err;
    }
    err = esp_ota_set_boot_partition(s_target);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA result: selecting %s failed (%s); boot partition unchanged",
                 s_target->label, esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "OTA result: %u bytes written and validated; %s boots next",
             (unsigned)s_received_bytes, s_target->label);
    return ESP_OK;
}

void ota_update_abort(void)
{
    if (!s_active) {
        return;
    }
    s_active = false;
    const esp_err_t err = esp_ota_abort(s_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_ota_abort: %s", esp_err_to_name(err));
    }
    ESP_LOGW(TAG, "OTA result: aborted after %u of %u bytes; boot partition unchanged",
             (unsigned)s_received_bytes, (unsigned)s_expected_bytes);
}

const char *ota_update_running_state_name(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t   state;
    if (running == NULL || esp_ota_get_state_partition(running, &state) != ESP_OK) {
        return "UNDEFINED (no otadata entry)";
    }
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
