/* =============================================================================
 * http_ota_server.h — Status page and image upload over HTTP
 * =============================================================================
 *   GET  /     plain-text status: version, running slot, OTA state, IP, uptime
 *   POST /ota  raw app image as the request body (Content-Length required),
 *              streamed into ota_update; on success the board restarts
 *              about a second after the reply.
 *   GET  /node/scan?from=0x700&to=0x71F  discover parked nodes (JSON)
 *   POST /node/flash?cmd=0x79E&reply=0x79F  stage a raw node .bin, then 202
 *   GET  /node/status  idle/staging/flashing/success/failed with bytes and %
 *
 * One static worker owns OTA, scan and node staging/flashing; overlapping
 * requests get 409. Status routes stay responsive. No authentication: bench
 * trusted LAN only. GPIO3 is not pulsed. See docs/bench-test.md section 9.
 *
 *   curl.exe --data-binary "@build/brain_ota.bin" http://<ip>/ota
 * ============================================================================= */

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


#ifdef __cplusplus
}
#endif

#endif /* HTTP_OTA_SERVER_H */
