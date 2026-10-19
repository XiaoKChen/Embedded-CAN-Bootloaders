/**
 * @file    can_updater_client.c
 * @brief   App-side entry into the ESP32-S3 CAN updater (see can_updater_client.h for the
 *          contract).
 *
 * Implementation notes: esp_restart() resets the CPU and the digital peripherals, so the caller's
 * TWAI/other drivers need no explicit deinit.
 *
 * @see     can_updater_client.h, recovery.c
 */

#include "can_updater_client.h"

#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"


/* -----------------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------------- */
esp_err_t can_updater_client_enter_updater(void)
{
    const esp_partition_t *updater = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    if (updater == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    const esp_err_t err = esp_ota_set_boot_partition(updater);
    if (err != ESP_OK) {
        return err;
    }

    esp_restart();
}
