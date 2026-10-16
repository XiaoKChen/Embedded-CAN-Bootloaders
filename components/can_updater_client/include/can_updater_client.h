/* =============================================================================
 * can_updater_client.h — App-side entry into the ESP32-S3 CAN updater
 * =============================================================================
 * The application's counterpart of `can_flash.py --enter-bootloader`: when
 * the app receives its "enter bootloader" request (on the Airform boards, the
 * /enter_bootloader route sent over ISO-TP), it calls
 * can_updater_client_enter_updater(). That points the boot partition at the
 * `factory` partition (the CAN updater) and restarts — the ESP32 equivalent
 * of the RA4M1 app writing the CAN-update handoff magic and resetting.
 *
 * Difference from the RA4M1: esp_ota_set_boot_partition(factory) erases
 * otadata, so the board stays in the updater across resets until an image
 * is flashed over CAN (or by esptool). It is never bricked: the updater is
 * always reachable.
 *
 * Requires the updater's partition table (partitions.csv) on the device.
 * ============================================================================= */

#ifndef CAN_UPDATER_CLIENT_H
#define CAN_UPDATER_CLIENT_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif


/* -----------------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------------- */

/**
 * Select the CAN updater (`factory` partition) as the boot partition and
 * restart into it. Does not return on success.
 *
 * @return ESP_ERR_NOT_FOUND when the partition table has no factory app, or
 *         the error from esp_ota_set_boot_partition; the app keeps running.
 */
esp_err_t can_updater_client_enter_updater(void);

/**
 * Call once, early in app_main (CPU0), before any deliberate esp_restart().
 * Registers a clean-restart shutdown mark and clears recovery counters after
 * 10 seconds of continuous uptime. A latched trip is cleared only in factory.
 * App sdkconfig must reserve 0x10 custom RTC bytes, excluded from IDF CRC,
 * exactly like node_updater/sdkconfig.defaults. Never call from an ISR.
 * Returns an ESP error if registration/task setup fails.
 */
esp_err_t can_updater_client_start_health(void);

/**
 * Factory updater only: after successful image VERIFY, clear both recovery
 * counters (read-back verifying flash) before GO selects/restarts the image.
 * This permits immediate recovery + flash without waiting for stable uptime.
 * Requires start_health() first. Failure must reject GO, not restart.
 */
esp_err_t can_updater_client_prepare_verified_boot(void);


#ifdef __cplusplus
}
#endif

#endif /* CAN_UPDATER_CLIENT_H */
