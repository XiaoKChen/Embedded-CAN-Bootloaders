/**
 * @file    node_flash.h
 * @brief   Brain CAN host and disposable node-image staging.
 *
 * Serves the brain's /node routes: stages a node image into node_fw, flashes it over CAN,
 * recovers a node, identifies a running node app and reports the last job. A node running an app
 * with the CAN service (examples/node_app) is parked with ENTER_UPDATER instead of a GPIO3 pulse
 * (can_update.h, app-control).
 *
 * Owns:         The CAN transport and host context, the node_fw staging and the last-job status.
 * Not here:     The worker that serializes jobs (http_ota_server.c); the GPIO3 recovery pulses
 *               (node_reset.h); the protocol (can_flash_host.h).
 * Invariants:   Power loss during erase/write/readback: only node_fw is affected; its length lives
 *               in RAM, so a partial/stale upload is never resumed after reset. During CAN
 *               DATA/VERIFY the node stays in its updater; GO selects its app.
 * Concurrency:  The HTTP server serializes scan/stage/run/recover with its own OTA on one worker.
 *               Status is safe to read concurrently.
 * Errors:       Handlers answer with HTTP statuses. POST /node/flash: 202 after staging, 400
 *               invalid IDs, 411 empty, 413 oversized, 408 timeout, 500 flash failure, 503
 *               unavailable. POST /node/recover: 202 accepted, 400 invalid IDs, 503 CAN
 *               unavailable. GET /node/status includes recovering, success and failed states.
 *               INFO retries can take over 8 s when no node responds.
 *
 * @see     node_flash.c, http_ota_server.h, docs/testing/bench/brain-can-flash.md
 */
#ifndef NODE_FLASH_H
#define NODE_FLASH_H

#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"
#include "can_flash_host.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Boot time granted to the updater after an app ACKed ENTER_UPDATER (restart,
 * 2nd-stage bootloader, updater TWAI bring-up) before INFO/HELLO is retried. */
#define NODE_FLASH_UPDATER_BOOT_MS 1500u

esp_err_t node_flash_init(void);
/* ENTER_UPDATER on the node's own IDs (worker only). On an app ACK (OK) it
 * waits NODE_FLASH_UPDATER_BOOT_MS so the caller can probe the updater next;
 * IN_UPDATER means the updater itself answered. Anything else: no CAN entry. */
can_flash_host_status_t node_flash_enter_updater(can_flash_host_t *host, uint16_t cmd_id, uint16_t reply_id);
/* Initialize another worker-owned host with the existing CAN transport only;
 * caller supplies read/progress/user. Does not reinitialize the peripheral. */
bool node_flash_transport(can_flash_host_t *host);
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
/* GET /node/app_info?cmd=&reply=: APP_INFO on the worker. 200 JSON with mode
 * "app" (project/version) or "updater" (parked), 400 invalid IDs, 502 invalid
 * reply, 504 no answer, 503 CAN unavailable. */
esp_err_t node_flash_app_info(httpd_req_t *req);
esp_err_t node_flash_status(httpd_req_t *req);

#ifdef __cplusplus
}
#endif
#endif /* NODE_FLASH_H */
