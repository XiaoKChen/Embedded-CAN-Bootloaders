/* =============================================================================
 * http_ota_server.c — Status page and image upload over HTTP
 * =============================================================================
 * See http_ota_server.h for the routes. The upload handler is only a
 * transport: it moves the request body into ota_update in fixed chunks
 * (never the whole image in RAM) and maps the outcome to an HTTP status.
 *
 * A failed upload never changes the boot partition:
 *   no / zero Content-Length   411, nothing written
 *   image larger than the slot 413, nothing written
 *   running image unconfirmed  409 (PENDING_VERIFY refuses esp_ota_begin)
 *   peer closed mid-body       aborted; no reply possible
 *   no data for RECV_TIMEOUT_S aborted (e.g. cable pulled); 408 attempted
 *   flash write / bad image    aborted; 400 or 500
 * ============================================================================= */

#include "http_ota_server.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "eth_w5500.h"
#include "ota_update.h"


/* -----------------------------------------------------------------------------
 * Constants
 * ----------------------------------------------------------------------------- */
#define SERVER_STACK_BYTES   8192u
#define RECV_TIMEOUT_S       10u    /* longest silence tolerated mid-upload   */
#define OTA_CHUNK_BYTES      4096u  /* one flash sector per esp_ota_write     */
#define STATUS_TEXT_BYTES    512u
#define REPLY_TEXT_BYTES     160u
#define RESTART_DELAY_MS     1000u  /* lets the 200 reply leave before reset  */
#define US_PER_S             1000000

#define STATUS_200           "200 OK"
#define STATUS_400           "400 Bad Request"
#define STATUS_408           "408 Request Timeout"
#define STATUS_409           "409 Conflict"
#define STATUS_411           "411 Length Required"
#define STATUS_413           "413 Payload Too Large"
#define STATUS_500           "500 Internal Server Error"

static const char *TAG = "http_ota";


/* -----------------------------------------------------------------------------
 * File-local state
 * ----------------------------------------------------------------------------- */
/* Static rather than on the server task's stack. Handlers run one at a time
 * on that single task, so one buffer serves every upload. */
static char s_chunk[OTA_CHUNK_BYTES];


/* -----------------------------------------------------------------------------
 * Private helpers
 * ----------------------------------------------------------------------------- */
static esp_err_t send_text(httpd_req_t *req, const char *status, const char *text)
{
    esp_err_t err = httpd_resp_set_status(req, status);
    if (err == ESP_OK) {
        err = httpd_resp_set_type(req, "text/plain");
    }
    if (err == ESP_OK) {
        err = httpd_resp_sendstr(req, text);
    }
    return err;
}

/* Rejects before anything is read: returning ESP_OK lets the server read and
 * discard the unread body, so the client still gets the reply. */
static esp_err_t reject_upload(httpd_req_t *req, const char *status, esp_err_t cause)
{
    ESP_LOGE(TAG, "OTA rejected: %s (%s); boot partition unchanged", status, esp_err_to_name(cause));
    char text[REPLY_TEXT_BYTES];
    (void)snprintf(text, sizeof(text), "OTA rejected: %s (%s)\n", status, esp_err_to_name(cause));
    (void)send_text(req, status, text);  /* best effort: the outcome is already decided */
    return ESP_OK;
}

static const char *begin_error_status(esp_err_t err)
{
    switch (err) {
    case ESP_ERR_INVALID_SIZE:               return STATUS_413;
    case ESP_ERR_OTA_ROLLBACK_INVALID_STATE: return STATUS_409;
    default:                                 return STATUS_500;
    }
}

static esp_err_t handle_status(httpd_req_t *req)
{
    const esp_app_desc_t  *desc    = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next    = esp_ota_get_next_update_partition(NULL);

    esp_netif_ip_info_t ip_info = { 0 };
    if (eth_w5500_get_ip_info(&ip_info) != ESP_OK) {
        ip_info.ip.addr = 0u;  /* shown as 0.0.0.0 */
    }

    char text[STATUS_TEXT_BYTES];
    (void)snprintf(text, sizeof(text),
                   "project:   %s\n"
                   "version:   %s\n"
                   "esp-idf:   %s\n"
                   "running:   %s (0x%" PRIx32 ")\n"
                   "ota state: %s\n"
                   "next slot: %s\n"
                   "ip:        " IPSTR "\n"
                   "uptime:    %" PRId64 " s\n",
                   desc->project_name, desc->version, desc->idf_ver,
                   (running != NULL) ? running->label : "?",
                   (running != NULL) ? running->address : 0u,
                   ota_update_running_state_name(),
                   (next != NULL) ? next->label : "none",
                   IP2STR(&ip_info.ip),
                   esp_timer_get_time() / US_PER_S);
    return send_text(req, STATUS_200, text);
}

static esp_err_t handle_ota(httpd_req_t *req)
{
    /* esp_http_server reports a missing Content-Length (or chunked encoding,
     * which it does not support) as content_len 0. */
    const size_t image_size = req->content_len;
    ESP_LOGI(TAG, "POST /ota: %u bytes announced", (unsigned)image_size);
    if (image_size == 0u) {
        return reject_upload(req, STATUS_411, ESP_ERR_INVALID_SIZE);
    }

    esp_err_t err = ota_update_begin(image_size);
    if (err != ESP_OK) {
        return reject_upload(req, begin_error_status(err), err);
    }

    size_t received = 0u;
    while (received < image_size) {
        const size_t wanted = (image_size - received < sizeof(s_chunk))
                            ? (image_size - received) : sizeof(s_chunk);
        const int got = httpd_req_recv(req, s_chunk, wanted);
        if (got == HTTPD_SOCK_ERR_TIMEOUT) {
            ESP_LOGE(TAG, "no data for %u s after %u of %u bytes (cable pulled?)",
                     RECV_TIMEOUT_S, (unsigned)received, (unsigned)image_size);
            ota_update_abort();
            (void)send_text(req, STATUS_408, "OTA aborted: receive timeout\n");  /* may not get out */
            return ESP_FAIL;  /* closes the socket */
        }
        if (got <= 0) {
            ESP_LOGE(TAG, "connection closed after %u of %u bytes",
                     (unsigned)received, (unsigned)image_size);
            ota_update_abort();
            return ESP_FAIL;  /* nobody left to reply to */
        }
        err = ota_update_write(s_chunk, (size_t)got);  /* aborts itself on error */
        if (err != ESP_OK) {
            char text[REPLY_TEXT_BYTES];
            (void)snprintf(text, sizeof(text), "OTA aborted at byte %u: %s\n",
                           (unsigned)received, esp_err_to_name(err));
            (void)send_text(req, (err == ESP_ERR_OTA_VALIDATE_FAILED) ? STATUS_400 : STATUS_500, text);
            return ESP_OK;  /* the server discards the rest of the body */
        }
        received += (size_t)got;
    }

    err = ota_update_finish();
    if (err != ESP_OK) {
        char text[REPLY_TEXT_BYTES];
        (void)snprintf(text, sizeof(text), "OTA rejected: %s; boot partition unchanged\n",
                       esp_err_to_name(err));
        return send_text(req, (err == ESP_ERR_OTA_VALIDATE_FAILED) ? STATUS_400 : STATUS_500, text);
    }

    char text[REPLY_TEXT_BYTES];
    (void)snprintf(text, sizeof(text), "OTA ok: %u bytes; rebooting into the new image\n",
                   (unsigned)image_size);
    (void)send_text(req, STATUS_200, text);  /* the new slot is selected either way */
    ESP_LOGI(TAG, "restarting in %u ms", RESTART_DELAY_MS);
    vTaskDelay(pdMS_TO_TICKS(RESTART_DELAY_MS));
    esp_restart();
    return ESP_OK;  /* not reached */
}


/* -----------------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------------- */
esp_err_t http_ota_server_start(void)
{
    httpd_config_t config    = HTTPD_DEFAULT_CONFIG();
    config.stack_size        = SERVER_STACK_BYTES;
    config.recv_wait_timeout = RECV_TIMEOUT_S;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        return err;
    }

    static const httpd_uri_t status_uri = {
        .uri = "/", .method = HTTP_GET, .handler = handle_status, .user_ctx = NULL,
    };
    static const httpd_uri_t ota_uri = {
        .uri = "/ota", .method = HTTP_POST, .handler = handle_ota, .user_ctx = NULL,
    };
    err = httpd_register_uri_handler(server, &status_uri);
    if (err == ESP_OK) {
        err = httpd_register_uri_handler(server, &ota_uri);
    }
    if (err != ESP_OK) {
        (void)httpd_stop(server);  /* reporting err, not the cleanup result */
        return err;
    }
    ESP_LOGI(TAG, "HTTP server on port %u: GET /, POST /ota", (unsigned)config.server_port);
    return ESP_OK;
}
