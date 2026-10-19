/**
 * @file    http_ota_server.h
 * @brief   Status page, image upload, node and package routes over HTTP.
 *
 * The brain's HTTP API (routes below): its own A/B update through ota_update, node discovery,
 * flashing and recovery through node_flash, and update packages through package_update.
 *
 * Owns:         The HTTP server, its routes and the one static worker that owns OTA, scan,
 *               app_info, node staging/flashing/recovery and packages.
 * Invariants:   No authentication: bench trusted LAN only. GPIO3 is pulsed HIGH then returned
 *               LOW; absent-node INFO can take over 8 s due to built-in retries.
 * Concurrency:  Overlapping requests get 409. Status routes stay responsive.
 * Errors:       http_ota_server_start() returns esp_err_t; routes answer with HTTP statuses.
 * Depends on:   eth_w5500.h, ota_update.h, node_flash.h, package_update.h.
 *
 * @see     http_ota_server.c, docs/reference/brain-http-api.md
 */

/*
 *   GET  /     plain-text status: version, running slot, OTA state, IP, uptime
 *   POST /ota  raw app image as the request body (Content-Length required),
 *              streamed into ota_update; on success the board restarts
 *              about a second after the reply.
 *   GET  /node/scan?from=0x700&to=0x71F  discover parked nodes (JSON)
 *   POST /node/flash?cmd=0x79E&reply=0x79F  stage a raw node .bin, then 202
 *   POST /node/recover?cmd=0x79E&reply=0x79F  pulse node EN, then probe INFO;
 *              202 accepted, 400 invalid IDs, 409 busy, 503 CAN unavailable
 *   GET  /node/app_info?cmd=0x79E&reply=0x79F  APP_INFO to a running node app:
 *              200 JSON {mode "app", project, version} or {mode "updater"},
 *              400 invalid IDs, 409 busy, 502 invalid reply, 504 no answer
 *   GET  /node/status  idle/staging/flashing/recovering/success/failed with bytes and %
 *   POST /package  stage/check a v1 package, then 202; brain first, nodes next
 *   GET  /package/status  receive/validation/brain/relay status and node results
 *
 *   curl.exe --data-binary "@build/brain_ota.bin" http://<ip>/ota
 *
 * A failed upload never changes the boot partition:
 *   no / zero Content-Length   411, nothing written
 *   image larger than the slot 413, nothing written
 *   running image unconfirmed  409 (PENDING_VERIFY refuses esp_ota_begin)
 *   peer closed mid-body       aborted; no reply possible
 *   no data for RECV_TIMEOUT_S aborted (e.g. cable pulled); 408 attempted
 *   flash write / bad image    aborted; 400 or 500
 */

#ifndef HTTP_OTA_SERVER_H
#define HTTP_OTA_SERVER_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif


/* -----------------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------------- */

/**
 * Start the HTTP server on port 80 and register the routes above.
 * Needs eth_w5500_start and node_flash_init to have been called.
 *
 * @return ESP_OK, or the error from httpd_start / handler registration.
 */
esp_err_t http_ota_server_start(void);
/* Call exactly once after the existing startup self-test. Schedules NVS/package
 * recovery on the same worker before accepting any new mutating HTTP job. */
void http_ota_server_resume_packages(void);


#ifdef __cplusplus
}
#endif

#endif /* HTTP_OTA_SERVER_H */
