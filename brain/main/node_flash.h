/* =============================================================================
 * node_flash.h — Brain CAN host and disposable node-image staging
 * =============================================================================
 * The HTTP server serializes scan/stage/run/recover with its own OTA on one
 * worker. Status is safe to read concurrently. Recovery uses node_reset to
 * pulse GPIO3; INFO retries can take over 8 s when no node responds.
 * Power loss during erase/write/readback: only node_fw is affected; its length
 * lives in RAM, so a partial/stale upload is never resumed after reset.
 * During CAN DATA/VERIFY the node stays in its updater; GO selects its app.
 * ============================================================================= */
#ifndef NODE_FLASH_H
#define NODE_FLASH_H

#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t node_flash_init(void);
esp_err_t node_flash_scan(httpd_req_t *req);
/* Sends 202 only after the complete body is staged and read-back verified.
 * ready is true only if that response was sent; caller completes/releases the
 * HTTP request before calling run. No CAN traffic on a failed upload. */
esp_err_t node_flash_stage(httpd_req_t *req, bool *ready);
void node_flash_run(void);
/* Sends 202 after validating IDs; worker completes the HTTP request before
 * pulsing the node and probing INFO. Invalid IDs return 400. */
esp_err_t node_flash_recover(httpd_req_t *req, bool *ready);
void node_flash_recover_run(void);
esp_err_t node_flash_status(httpd_req_t *req);

#ifdef __cplusplus
}
#endif
#endif /* NODE_FLASH_H */
