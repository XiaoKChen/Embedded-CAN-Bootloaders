/* =============================================================================
 * http_ota_server.h — Status page and image upload over HTTP
 * =============================================================================
 *   GET  /     plain-text status: version, running slot, OTA state, IP, uptime
 *   POST /ota  raw app image as the request body (Content-Length required),
 *              streamed into ota_update; on success the board restarts
 *              about a second after the reply.
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
 * Start the HTTP server on port 80 and register GET / and POST /ota.
 * Needs the network stack (eth_w5500_start) to be up.
 *
 * @return ESP_OK, or the error from httpd_start / handler registration.
 */
esp_err_t http_ota_server_start(void);


#ifdef __cplusplus
}
#endif

#endif /* HTTP_OTA_SERVER_H */
