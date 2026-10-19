/**
 * @file    http_ota_server.c
 * @brief   Status page and image upload over HTTP (see http_ota_server.h for the contract).
 *
 * Implementation notes: the upload handler is only a transport — it moves the request body into
 * ota_update in fixed chunks (never the whole image in RAM) and maps the outcome to an HTTP
 * status. GET /node/app_info and node recovery run on the same worker as every other job; an
 * absent node can take over 8 s to fail INFO because the host retries internally.
 *
 * @see     http_ota_server.h, ota_update.h, node_flash.h, package_update.h
 */

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
#include "node_flash.h"
#include "package_update.h"


/* -----------------------------------------------------------------------------
 * Constants
 * ----------------------------------------------------------------------------- */
#define SERVER_STACK_BYTES   8192u
#define WORKER_STACK_BYTES   8192u
#define WORKER_PRIORITY      5u
#define RECV_TIMEOUT_S       10u    /* longest silence tolerated mid-upload   */
#define OTA_CHUNK_BYTES      4096u  /* one flash sector per esp_ota_write     */
#define STATUS_TEXT_BYTES    512u
#define REPLY_TEXT_BYTES     160u
#define RESTART_DELAY_MS     1000u  /* lets the 200 reply leave before reset  */
#define US_PER_S             1000000
#define URI_HANDLER_COUNT    9u     /* routes registered below (default 8)    */

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
/* One static worker owns all flash mutations and CAN operations. The HTTP
 * task stays free to answer status and reject conflicting requests with 409. */
static char s_chunk[OTA_CHUNK_BYTES];
static StaticTask_t s_worker_tcb;
static StackType_t s_worker_stack[WORKER_STACK_BYTES / sizeof(StackType_t)];
static TaskHandle_t s_worker;
static portMUX_TYPE s_job_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_busy;
typedef enum { JOB_OTA, JOB_SCAN, JOB_NODE_FLASH, JOB_NODE_RECOVER, JOB_NODE_APP_INFO, JOB_PACKAGE,
               JOB_RESUME_PACKAGE } job_type_t;
static job_type_t s_job;
static httpd_req_t *s_request;


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

/* Reject before reading anything. The worker closes its async connection
 * after the response rather than draining a rejected image. */
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
            return ESP_OK;  /* the worker closes without draining the rest */
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


static void release_job(void)
{
    portENTER_CRITICAL(&s_job_lock);
    s_busy = false;
    portEXIT_CRITICAL(&s_job_lock);
}

static esp_err_t submit_job(httpd_req_t *req, job_type_t job)
{
    portENTER_CRITICAL(&s_job_lock);
    const bool busy = s_busy;
    if (!busy) { s_busy = true; }
    portEXIT_CRITICAL(&s_job_lock);
    /* Reserve ownership before reading the retained-record gate. Otherwise a
     * worker could fail its NVS commit and release between the two snapshots. */
    if (busy || package_update_blocked()) {
        if (!busy) { release_job(); }
        /* Close instead of draining a rejected multi-MB upload on the HTTP
         * task, so status and further conflict replies remain responsive. */
        (void)send_text(req, STATUS_409, "An update/scan/recovery, startup self-test, or retained package job is active; poll status\n"); /* best effort */
        return ESP_FAIL;
    }
    httpd_req_t *copy = NULL;
    const esp_err_t err = httpd_req_async_handler_begin(req, &copy);
    if (err != ESP_OK) {
        release_job();
        (void)send_text(req, STATUS_500, "Cannot start asynchronous request\n"); /* best effort */
        return ESP_FAIL;
    }
    portENTER_CRITICAL(&s_job_lock);
    s_request = copy;
    s_job = job;
    portEXIT_CRITICAL(&s_job_lock);
    xTaskNotifyGive(s_worker);
    return ESP_OK;
}

static esp_err_t submit_package(httpd_req_t *req) { return submit_job(req, JOB_PACKAGE); }
static esp_err_t submit_ota(httpd_req_t *req) { return submit_job(req, JOB_OTA); }
static esp_err_t submit_scan(httpd_req_t *req) { return submit_job(req, JOB_SCAN); }
static esp_err_t submit_node_flash(httpd_req_t *req) { return submit_job(req, JOB_NODE_FLASH); }
static esp_err_t submit_node_recover(httpd_req_t *req) { return submit_job(req, JOB_NODE_RECOVER); }
static esp_err_t submit_node_app_info(httpd_req_t *req) { return submit_job(req, JOB_NODE_APP_INFO); }

static void request_worker(void *arg)
{
    (void)arg;
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY); /* HTTP jobs or startup resume */
        portENTER_CRITICAL(&s_job_lock);
        httpd_req_t *req = s_request;
        const job_type_t job = s_job;
        portEXIT_CRITICAL(&s_job_lock);
        if (job == JOB_RESUME_PACKAGE) {
            package_update_resume();
            release_job();
            continue;
        }
        bool ready = false;
        esp_err_t err = httpd_resp_set_hdr(req, "Connection", "close");
        if (err == ESP_OK) {
            if (job == JOB_OTA) { err = handle_ota(req); }
            else if (job == JOB_SCAN) { err = node_flash_scan(req); }
            else if (job == JOB_NODE_FLASH) { err = node_flash_stage(req, &ready); }
            else if (job == JOB_PACKAGE) { err = package_update_stage(req, &ready); }
            else if (job == JOB_NODE_APP_INFO) { err = node_flash_app_info(req); }
            else { err = node_flash_recover(req, &ready); }
        }
        if (err != ESP_OK) { ESP_LOGW(TAG, "request ended: %s", esp_err_to_name(err)); }
        const httpd_handle_t server = req->handle;
        const int socket = httpd_req_to_sockfd(req);
        /* Async requests do not inherit the synchronous handler's automatic
         * error cleanup or unread-body drain. Always release and close ours. */
        const esp_err_t complete = httpd_req_async_handler_complete(req);
        if (complete != ESP_OK) { ESP_LOGE(TAG, "async completion: %s", esp_err_to_name(complete)); }
        const esp_err_t close = httpd_sess_trigger_close(server, socket);
        if (close != ESP_OK && close != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "session close: %s", esp_err_to_name(close));
        }
        if (ready && job == JOB_PACKAGE) { package_update_run(); }
        if (ready && job == JOB_NODE_FLASH) { node_flash_run(); }
        if (ready && job == JOB_NODE_RECOVER) { node_flash_recover_run(); }
        release_job();
    }
}

/* -----------------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------------- */
void http_ota_server_resume_packages(void)
{
    if (s_worker == NULL) { return; }
    /* Startup owns the reserved worker until confirm_image has finished. */
    portENTER_CRITICAL(&s_job_lock);
    s_job = JOB_RESUME_PACKAGE;
    s_request = NULL;
    portEXIT_CRITICAL(&s_job_lock);
    xTaskNotifyGive(s_worker);
}

esp_err_t http_ota_server_start(void)
{
    if (s_worker != NULL) { return ESP_ERR_INVALID_STATE; }
    s_busy = true;
    s_worker = xTaskCreateStatic(request_worker, "update_worker", WORKER_STACK_BYTES,
                                 NULL, WORKER_PRIORITY, s_worker_stack, &s_worker_tcb);
    if (s_worker == NULL) { return ESP_ERR_NO_MEM; }
    httpd_config_t config    = HTTPD_DEFAULT_CONFIG();
    config.stack_size        = SERVER_STACK_BYTES;
    config.recv_wait_timeout = RECV_TIMEOUT_S;
    config.max_uri_handlers  = URI_HANDLER_COUNT;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        vTaskDelete(s_worker);
        s_worker = NULL;
        return err;
    }

    static const httpd_uri_t status_uri = {
        .uri = "/", .method = HTTP_GET, .handler = handle_status, .user_ctx = NULL,
    };
    static const httpd_uri_t ota_uri = {
        .uri = "/ota", .method = HTTP_POST, .handler = submit_ota, .user_ctx = NULL,
    };
    static const httpd_uri_t scan_uri = {
        .uri = "/node/scan", .method = HTTP_GET, .handler = submit_scan,
    };
    static const httpd_uri_t flash_uri = {
        .uri = "/node/flash", .method = HTTP_POST, .handler = submit_node_flash,
    };
    static const httpd_uri_t recover_uri = {
        .uri = "/node/recover", .method = HTTP_POST, .handler = submit_node_recover,
    };
    static const httpd_uri_t app_info_uri = {
        .uri = "/node/app_info", .method = HTTP_GET, .handler = submit_node_app_info,
    };
    static const httpd_uri_t node_status_uri = {
        .uri = "/node/status", .method = HTTP_GET, .handler = node_flash_status,
    };
    static const httpd_uri_t package_uri = {
        .uri = "/package", .method = HTTP_POST, .handler = submit_package,
    };
    static const httpd_uri_t package_status_uri = {
        .uri = "/package/status", .method = HTTP_GET, .handler = package_update_status,
    };
    err = httpd_register_uri_handler(server, &status_uri);
    if (err == ESP_OK) {
        err = httpd_register_uri_handler(server, &ota_uri);
    }
    if (err == ESP_OK) { err = httpd_register_uri_handler(server, &scan_uri); }
    if (err == ESP_OK) { err = httpd_register_uri_handler(server, &flash_uri); }
    if (err == ESP_OK) { err = httpd_register_uri_handler(server, &recover_uri); }
    if (err == ESP_OK) { err = httpd_register_uri_handler(server, &app_info_uri); }
    if (err == ESP_OK) { err = httpd_register_uri_handler(server, &node_status_uri); }
    if (err == ESP_OK) { err = httpd_register_uri_handler(server, &package_uri); }
    if (err == ESP_OK) { err = httpd_register_uri_handler(server, &package_status_uri); }
    if (err != ESP_OK) {
        (void)httpd_stop(server);  /* reporting err, not the cleanup result */
        vTaskDelete(s_worker);
        s_worker = NULL;
        return err;
    }
    ESP_LOGI(TAG, "HTTP port %u: GET /, POST /ota, GET /node/scan, POST /node/flash, POST /node/recover, GET /node/app_info, GET /node/status, POST /package, GET /package/status",
             (unsigned)config.server_port);
    return ESP_OK;
}
