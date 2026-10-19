/**
 * @file    can_service.h
 * @brief   The node app's side of the CAN update protocol.
 *
 * Brings TWAI up on the node's update IDs and runs one task that answers the app-control
 * extension (APP_INFO, ENTER_UPDATER; src/boot/can_update.h) so a host can identify the running
 * app and park it in the CAN updater without the GPIO3 reset line. Copy this file pair into real
 * node firmware as is.
 *
 * Owns:         TWAI on the node's update IDs and the service task.
 * Not here:     Application traffic: it needs its own IDs and, because the link filters on the
 *               cmd ID in hardware, its own can_link setup or a wider filter.
 * Invariants:   The task never answers updater commands (HELLO, INFO, SIZE, ...): a running app is
 *               not an updater, and staying silent is what tells the brain to send ENTER_UPDATER.
 * Errors:       can_service_start() returns esp_err_t; can_service_load_ids() never fails.
 * Depends on:   app_control.h, can_link.h, can_updater_client.h, dfu_config.h.
 *
 * @see     can_service.c, node_app.c, docs/guides/writing-a-node-app.md
 */

#ifndef CAN_SERVICE_H
#define CAN_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif


/* -----------------------------------------------------------------------------
 * Types
 * ----------------------------------------------------------------------------- */
typedef struct {
    uint16_t cmd_id;       /* host -> node, the ID the task listens on        */
    uint16_t reply_id;     /* node -> host                                    */
    bool     custom;       /* not 0x79E/0x79F: a v2 `cancfg` block set them  */
} can_service_ids_t;


/* -----------------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------------- */

/**
 * The node's update IDs, exactly as the updater derives them: the `cancfg`
 * partition's v2 block when provisioned, else 0x79E / 0x79F. Never fails.
 */
void can_service_load_ids(can_service_ids_t *ids);

/**
 * Start TWAI (CONFIG_NODE_APP_* pins and bitrate, hardware filter on
 * ids->cmd_id) and the service task. Call once.
 *
 * @return ESP_OK; ESP_FAIL when the TWAI driver did not start;
 *         ESP_ERR_NO_MEM when the task could not be created.
 */
esp_err_t can_service_start(const can_service_ids_t *ids);


#ifdef __cplusplus
}
#endif

#endif /* CAN_SERVICE_H */
