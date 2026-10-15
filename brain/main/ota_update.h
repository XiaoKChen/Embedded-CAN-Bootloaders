/* =============================================================================
 * ota_update.h — Transport-independent A/B self-update of the running app
 * =============================================================================
 * Writes an app image, chunk by chunk, into the inactive OTA slot and, once
 * the whole image is in and validated, makes that slot the boot partition.
 * It knows nothing about where the bytes come from: the HTTP handler
 * (http_ota_server.c) is one caller; Wi-Fi, BLE or CAN can be others.
 *
 *   ota_update_begin(size)       once, with the exact image size
 *   ota_update_write(chunk, len) any number of times, in order
 *   ota_update_finish()          validates, then switches the boot slot
 *   ota_update_abort()           on any transport error; a no-op when idle
 *
 * Any failure inside write/finish ends the session itself (the caller only
 * reports it); the boot partition changes only in a successful finish.
 *
 * Power loss at each step (rules/bootloader.md, fail-safe):
 *   begin / write   only the inactive slot is erased and written, sector by
 *                   sector; otadata is untouched, so the running image boots.
 *   finish: end     reads back and validates the slot; nothing else written.
 *   finish: set     esp_ota_set_boot_partition writes the next otadata
 *   boot partition  sector (sequence + CRC). Interrupted, its CRC fails and
 *                   the bootloader uses the other sector: the old image.
 *   after finish    the new image boots PENDING_VERIFY; if it resets before
 *                   it is marked valid, the bootloader rolls back.
 *
 * Not thread-safe: one caller at a time. The HTTP server serializes mutations
 * with node staging/flashing on its single update worker.
 * ============================================================================= */

#ifndef OTA_UPDATE_H
#define OTA_UPDATE_H

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif


/* -----------------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------------- */

/**
 * Start writing an image of exactly image_size bytes into the inactive slot.
 *
 * @return ESP_OK; ESP_ERR_INVALID_STATE if an update is already in progress;
 *         ESP_ERR_INVALID_SIZE if image_size is 0 or larger than the slot;
 *         ESP_ERR_OTA_ROLLBACK_INVALID_STATE while the running image is still
 *         PENDING_VERIFY; otherwise the error from esp_ota_begin.
 */
esp_err_t ota_update_begin(size_t image_size);

/**
 * Append the next chunk of the image. On error the update is aborted.
 *
 * @return ESP_OK; ESP_ERR_INVALID_STATE with no update in progress;
 *         ESP_ERR_INVALID_SIZE if the chunk overruns the announced size;
 *         ESP_ERR_OTA_VALIDATE_FAILED if the first byte is not an image
 *         magic; otherwise the flash error from esp_ota_write.
 */
esp_err_t ota_update_write(const void *chunk, size_t len);

/**
 * Validate the complete image and make its slot the boot partition. The
 * update is over whatever the result; the caller restarts on ESP_OK.
 *
 * @return ESP_OK; ESP_ERR_INVALID_STATE with no update in progress;
 *         ESP_ERR_INVALID_SIZE if fewer bytes arrived than announced;
 *         ESP_ERR_OTA_VALIDATE_FAILED if the image is rejected; otherwise
 *         the error from esp_ota_set_boot_partition.
 */
esp_err_t ota_update_finish(void);

/** Abandon the update in progress, if any. The boot partition is unchanged. */
void ota_update_abort(void);

/** Name of the running image's OTA state ("VALID", "PENDING_VERIFY", ...). */
const char *ota_update_running_state_name(void);


#ifdef __cplusplus
}
#endif

#endif /* OTA_UPDATE_H */
