/* =============================================================================
 * node_flash.c — Brain staging, CAN host, and node recovery
 * =============================================================================
 * POST /node/flash: 202 after staging, 400 invalid IDs, 411 empty, 413 oversized,
 *                   408 timeout, 500 flash failure, 503 unavailable.
 * POST /node/recover: 202 accepted, 400 invalid IDs, 503 CAN unavailable.
 * GET /node/status: includes recovering, success and failed states.
 * GET /node/app_info: identifies a running node app (see node_flash.h).
 * /node/flash sends ENTER_UPDATER before HELLO: a running app parks itself
 * (then the updater gets its boot time), the updater NAKs it at once.
 * INFO has four built-in 2 s retries; an absent node can take over 8 s to fail.
 * ============================================================================= */
#include "node_flash.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "can_flash_host.h"
#include "can_link.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "node_reset.h"

#define NODE_FW_LABEL       "node_fw"
#define NODE_FW_SUBTYPE     ((esp_partition_subtype_t)0x41)
#define CAN_TX_GPIO         6
#define CAN_RX_GPIO         7
#define CAN_BITRATE         250000u
#define CAN_RX_QUEUE        64u
#define STAGE_CHUNK_BYTES   4096u
#define NODE_STATUS_BYTES   192u
#define NODE_JSON_BYTES     384u
#define SCAN_MAX_NODES      64u
#define QUERY_BYTES         96u
#define ID_TEXT_BYTES       16u
#define US_PER_MS           1000u
#define PERCENT_MAX         100u
#define JSON_ESCAPE_BYTES   6u
#define RECOVERY_BOOT_MS    1000u
#define APP_TEXT_BYTES      (APP_CONTROL_STR_MAX * JSON_ESCAPE_BYTES + 1u)
#define APP_JSON_BYTES      (2u * APP_TEXT_BYTES + 128u)  /* both strings fully escaped */

static const char *TAG = "node_flash";
static const esp_partition_t *s_partition;
static bool s_link_ready;
static can_flash_host_t s_host;
static uint8_t s_chunk[STAGE_CHUNK_BYTES];
static uint8_t s_readback[STAGE_CHUNK_BYTES];
static can_flash_host_info_t s_found[SCAN_MAX_NODES];
static uint16_t s_cmd_id, s_reply_id;
static uint32_t s_image_size;

typedef enum {
    NODE_IDLE = 0, NODE_STAGING, NODE_FLASHING, NODE_RECOVERING, NODE_SUCCESS, NODE_FAILED,
} node_state_t;

typedef struct {
    node_state_t state;
    uint32_t bytes;
    uint32_t total;
    char text[NODE_STATUS_BYTES];
} node_status_t;

static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;
static node_status_t s_status;

static void set_status(node_state_t state, uint32_t bytes, uint32_t total, const char *text)
{
    node_status_t next = { .state = state, .bytes = bytes, .total = total };
    (void)snprintf(next.text, sizeof(next.text), "%s", text);
    portENTER_CRITICAL(&s_status_lock);
    s_status = next;
    portEXIT_CRITICAL(&s_status_lock);
}

static esp_err_t respond(httpd_req_t *req, const char *status, const char *text)
{
    esp_err_t err = httpd_resp_set_status(req, status);
    if (err == ESP_OK) { err = httpd_resp_set_type(req, "text/plain"); }
    if (err == ESP_OK) { err = httpd_resp_sendstr(req, text); }
    return err;
}

static uint32_t clock_ms(void *user)
{
    (void)user;
    return (uint32_t)((uint64_t)esp_timer_get_time() / US_PER_MS);
}

static can_flash_host_io_t send_frame(void *user, const can_flash_host_frame_t *frame, uint32_t timeout)
{
    const uint32_t start = clock_ms(user);
    do {
        const can_link_status_t status = can_link_send(frame->id, frame->data);
        if (status == CAN_LINK_STATUS_OK) { return CAN_FLASH_HOST_IO_OK; }
        if (status == CAN_LINK_STATUS_INVALID_ARG) { return CAN_FLASH_HOST_IO_ERROR; }
        /* can_link waits 50 ms for queue space; do not spin during bus-off. */
        vTaskDelay(1u);
    } while (clock_ms(user) - start < timeout);
    return CAN_FLASH_HOST_IO_TIMEOUT;
}

static can_flash_host_io_t receive_frame(void *user, can_flash_host_frame_t *frame, uint32_t timeout)
{
    (void)user;
    can_link_frame_t rx;
    /* can_link truncates milliseconds to ticks. Round UP so the essential
     * 1 ms early poll really yields even with the default 100 Hz tick. */
    const uint32_t wait = timeout == 0u ? 0u :
        ((timeout + portTICK_PERIOD_MS - 1u) / portTICK_PERIOD_MS) * portTICK_PERIOD_MS;
    const can_link_status_t status = can_link_receive_frame(&rx, wait);
    if (status == CAN_LINK_STATUS_TIMEOUT) { return CAN_FLASH_HOST_IO_TIMEOUT; }
    if (status != CAN_LINK_STATUS_OK) { return CAN_FLASH_HOST_IO_ERROR; }
    *frame = (can_flash_host_frame_t){ .id = rx.id, .dlc = rx.dlc,
        .extended = rx.extended, .remote = rx.remote };
    memcpy(frame->data, rx.data, sizeof(frame->data));
    return CAN_FLASH_HOST_IO_OK;
}

static bool read_image(void *user, uint32_t offset, uint8_t *buf, uint32_t len)
{
    (void)user;
    return offset <= s_image_size && len <= s_image_size - offset &&
           esp_partition_read(s_partition, offset, buf, len) == ESP_OK;
}

static void flash_progress(void *user, uint32_t bytes, uint32_t total)
{
    (void)user;
    set_status(NODE_FLASHING, bytes, total, "CAN transfer; awaiting VERIFY and GO");
}

/* Decimal or 0x-prefixed hexadecimal only; no signs, whitespace, truncation,
 * implicit octal, URL encoding, or values outside the standard CAN ID range. */
static bool query_id(const char *query, const char *key, uint16_t *id)
{
    char text[ID_TEXT_BYTES];
    const esp_err_t err = httpd_query_key_value(query, key, text, sizeof(text));
    if (err == ESP_ERR_NOT_FOUND) { return true; }
    if (err != ESP_OK || text[0] == '\0') { return false; }
    uint32_t value = 0u;
    uint32_t base = 10u;
    const char *p = text;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { base = 16u; p += 2; }
    if (*p == '\0') { return false; }
    for (; *p != '\0'; ++p) {
        uint32_t digit;
        if (*p >= '0' && *p <= '9') { digit = (uint32_t)(*p - '0'); }
        else if (*p >= 'a' && *p <= 'f') { digit = (uint32_t)(*p - 'a') + 10u; }
        else if (*p >= 'A' && *p <= 'F') { digit = (uint32_t)(*p - 'A') + 10u; }
        else { return false; }
        if (digit >= base || value > (DFU_CONFIG_CAN_ID_MAX - digit) / base) { return false; }
        value = value * base + digit;
    }
    *id = (uint16_t)value;
    return true;
}

static bool get_ids(httpd_req_t *req, const char *key_a, uint16_t *a, const char *key_b, uint16_t *b)
{
    char query[QUERY_BYTES] = { 0 };
    const esp_err_t err = httpd_req_get_url_query_str(req, query, sizeof(query));
    if (err == ESP_ERR_NOT_FOUND) { return true; }
    return err == ESP_OK && query_id(query, key_a, a) && query_id(query, key_b, b);
}

esp_err_t node_flash_init(void)
{
    s_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, NODE_FW_SUBTYPE, NODE_FW_LABEL);
    s_host = (can_flash_host_t){ .send = send_frame, .receive = receive_frame,
        .clock_ms = clock_ms, .read = read_image, .progress = flash_progress };
    const can_link_config_t cfg = { .tx_gpio = CAN_TX_GPIO, .rx_gpio = CAN_RX_GPIO,
        .bitrate_bps = CAN_BITRATE, .accept_all = true, .rx_queue_len = CAN_RX_QUEUE };
    s_link_ready = can_link_init(&cfg) == CAN_LINK_STATUS_OK;
    if (!s_link_ready) { return ESP_FAIL; }
    ESP_LOGI(TAG, "TWAI TX%d/RX%d, %u bit/s, broad discovery RX", CAN_TX_GPIO, CAN_RX_GPIO, CAN_BITRATE);
    return s_partition == NULL ? ESP_ERR_NOT_FOUND : ESP_OK;
}

bool node_flash_transport(can_flash_host_t *host)
{
    if (!s_link_ready || host == NULL) { return false; }
    *host = (can_flash_host_t){ .send = send_frame, .receive = receive_frame, .clock_ms = clock_ms };
    return true;
}

esp_err_t node_flash_scan(httpd_req_t *req)
{
    if (!s_link_ready) { return respond(req, "503 Service Unavailable", "CAN link unavailable\n"); }
    uint16_t from = CAN_FLASH_HOST_SCAN_FROM, to = CAN_FLASH_HOST_SCAN_TO;
    if (!get_ids(req, "from", &from, "to", &to) || from > to) {
        return respond(req, "400 Bad Request", "Expected 0 <= from <= to <= 0x7FF\n");
    }
    uint32_t count;
    const can_flash_host_status_t status = can_flash_host_scan(&s_host, from, to, s_found, SCAN_MAX_NODES, &count);
    if (status != CAN_FLASH_HOST_STATUS_OK && status != CAN_FLASH_HOST_STATUS_CAPACITY) {
        char text[NODE_STATUS_BYTES];
        can_flash_host_result_text(&s_host.result, text, sizeof(text));
        return respond(req, "502 Bad Gateway", text);
    }
    esp_err_t err = httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK) { err = httpd_resp_sendstr_chunk(req, "{\"nodes\":["); }
    for (uint32_t i = 0u; i < count && err == ESP_OK; ++i) {
        /* Encode every byte, including quotes/control bytes, as JSON Unicode.
         * Provisioning says ASCII; non-ASCII follows Python's replacement. */
        char name[DFU_CONFIG_STR_MAX * JSON_ESCAPE_BYTES + 1u];
        for (uint32_t j = 0u; j < s_found[i].name_len; ++j) {
            const uint8_t byte = (uint8_t)s_found[i].name[j];
            (void)snprintf(&name[j * JSON_ESCAPE_BYTES], JSON_ESCAPE_BYTES + 1u,
                           "\\u%04x", byte < 0x80u ? byte : 0xFFFDu);
        }
        name[s_found[i].name_len * JSON_ESCAPE_BYTES] = '\0';
        char json[NODE_JSON_BYTES];
        (void)snprintf(json, sizeof(json), "%s{\"cmd_id\":%u,\"reply_id\":%u,\"name\":\"%s\",\"protocol\":%u}",
                       i == 0u ? "" : ",", s_found[i].cmd_id, s_found[i].reply_id, name, s_found[i].protocol_version);
        err = httpd_resp_sendstr_chunk(req, json);
    }
    if (err == ESP_OK) {
        err = httpd_resp_sendstr_chunk(req, status == CAN_FLASH_HOST_STATUS_CAPACITY ?
            "],\"truncated\":true}\n" : "],\"truncated\":false}\n");
    }
    if (err == ESP_OK) { err = httpd_resp_sendstr_chunk(req, NULL); }
    return err;
}

static esp_err_t stage_failed(httpd_req_t *req, const char *http_status, const char *text, uint32_t received)
{
    set_status(NODE_FAILED, received, s_image_size, text);
    s_image_size = 0u; /* incomplete data is never an eligible CAN source */
    return respond(req, http_status, text);
}

esp_err_t node_flash_stage(httpd_req_t *req, bool *ready)
{
    *ready = false;
    if (!s_link_ready || s_partition == NULL || s_partition->encrypted || s_partition->readonly) {
        return respond(req, "503 Service Unavailable", "CAN or writable unencrypted node_fw partition unavailable\n");
    }
    s_cmd_id = CAN_ID_HOST_TO_BOOT;
    s_reply_id = CAN_ID_BOOT_TO_HOST;
    if (!get_ids(req, "cmd", &s_cmd_id, "reply", &s_reply_id)) {
        return respond(req, "400 Bad Request", "cmd/reply must be standard CAN IDs (0..0x7FF)\n");
    }
    if (req->content_len == 0u) { return respond(req, "411 Length Required", "Nonempty Content-Length required\n"); }
    if (req->content_len > s_partition->size) { return respond(req, "413 Payload Too Large", "Image exceeds node_fw partition\n"); }
    s_image_size = (uint32_t)req->content_len;
    set_status(NODE_STAGING, 0u, s_image_size, "Receiving into node_fw");
    uint32_t received = 0u;
    uint32_t erased = 0u;
    while (received < s_image_size) {
        const uint32_t left = s_image_size - received;
        const int got = httpd_req_recv(req, (char *)s_chunk, left < sizeof(s_chunk) ? left : sizeof(s_chunk));
        if (got == HTTPD_SOCK_ERR_TIMEOUT) { return stage_failed(req, "408 Request Timeout", "Upload receive timeout", received); }
        if (got <= 0) {
            set_status(NODE_FAILED, received, s_image_size, "Upload connection closed");
            s_image_size = 0u;
            return ESP_FAIL;
        }
        const uint32_t end = received + (uint32_t)got;
        while (erased < end) {
            if (s_partition->erase_size == 0u || s_partition->erase_size > s_partition->size - erased ||
                esp_partition_erase_range(s_partition, erased, s_partition->erase_size) != ESP_OK) {
                return stage_failed(req, "500 Internal Server Error", "node_fw erase failed", received);
            }
            erased += s_partition->erase_size;
        }
        if (esp_partition_write(s_partition, received, s_chunk, (size_t)got) != ESP_OK ||
            esp_partition_read(s_partition, received, s_readback, (size_t)got) != ESP_OK ||
            memcmp(s_chunk, s_readback, (size_t)got) != 0) {
            return stage_failed(req, "500 Internal Server Error", "node_fw write/read-back failed", received);
        }
        received = end;
        set_status(NODE_STAGING, received, s_image_size, "Receiving into node_fw");
    }
    const esp_err_t err = respond(req, "202 Accepted", "Node image staged; poll /node/status\n");
    if (err != ESP_OK) {
        set_status(NODE_FAILED, received, s_image_size, "Upload response connection closed; CAN not started");
        s_image_size = 0u;
        return err;
    }
    *ready = true;
    return ESP_OK;
}

can_flash_host_status_t node_flash_enter_updater(can_flash_host_t *host, uint16_t cmd_id, uint16_t reply_id)
{
    const can_flash_host_status_t status = can_flash_host_enter_updater(host, cmd_id, reply_id);
    char text[NODE_STATUS_BYTES];
    can_flash_host_result_text(&host->result, text, sizeof(text));
    ESP_LOGI(TAG, "ENTER_UPDATER cmd 0x%03X reply 0x%03X: %s", cmd_id, reply_id, text);
    if (status == CAN_FLASH_HOST_STATUS_OK) {
        vTaskDelay(pdMS_TO_TICKS(NODE_FLASH_UPDATER_BOOT_MS));
    }
    return status;
}

void node_flash_run(void)
{
    /* ENTER_UPDATER first: a running app parks itself, the updater NAKs it at
     * once, and silence costs only its short retries before HELLO. */
    set_status(NODE_FLASHING, 0u, s_image_size, "Sending ENTER_UPDATER over CAN");
    const bool entered = node_flash_enter_updater(&s_host, s_cmd_id, s_reply_id) == CAN_FLASH_HOST_STATUS_OK;
    const char *path = entered ? " (entered updater via CAN)" : "";
    set_status(NODE_FLASHING, 0u, s_image_size, entered ? "CAN handshake (entered updater via CAN)" : "CAN handshake");
    can_flash_host_result_t result;
    const can_flash_host_status_t status = can_flash_host_flash(&s_host, s_cmd_id, s_reply_id, s_image_size, &result);
    char diagnostic[NODE_STATUS_BYTES - sizeof(" (entered updater via CAN)")];
    can_flash_host_result_text(&result, diagnostic, sizeof(diagnostic));
    char text[NODE_STATUS_BYTES];
    (void)snprintf(text, sizeof(text), "%s%s", diagnostic, path);
    set_status(status == CAN_FLASH_HOST_STATUS_OK ? NODE_SUCCESS : NODE_FAILED, result.bytes, s_image_size, text);
    ESP_LOGI(TAG, "node cmd 0x%03X reply 0x%03X: %s", s_cmd_id, s_reply_id, text);
    can_link_health_t health;
    if (can_link_get_health(&health) == CAN_LINK_STATUS_OK) {
        ESP_LOGI(TAG, "CAN health: state %u, TX errors %" PRIu32 ", RX errors %" PRIu32 ", missed %" PRIu32 ", overrun %" PRIu32,
                 (unsigned)health.state, health.tx_error_counter, health.rx_error_counter, health.rx_missed_count, health.rx_overrun_count);
    }
}

esp_err_t node_flash_recover(httpd_req_t *req, bool *ready)
{
    *ready = false;
    if (!s_link_ready) { return respond(req, "503 Service Unavailable", "CAN link unavailable\n"); }
    s_cmd_id = CAN_ID_HOST_TO_BOOT;
    s_reply_id = CAN_ID_BOOT_TO_HOST;
    if (!get_ids(req, "cmd", &s_cmd_id, "reply", &s_reply_id)) {
        return respond(req, "400 Bad Request", "cmd/reply must be standard CAN IDs (0..0x7FF)\n");
    }
    const esp_err_t err = respond(req, "202 Accepted", "Recovery started; poll /node/status\n");
    if (err == ESP_OK) {
        set_status(NODE_RECOVERING, 0u, 0u, "Recovery pulses; awaiting INFO");
        *ready = true;
    }
    return err;
}

void node_flash_recover_run(void)
{
    const esp_err_t err = node_reset_pulse_train(NODE_RESET_RECOVERY_PULSES,
        NODE_RESET_RECOVERY_HIGH_MS, NODE_RESET_RECOVERY_LOW_MS);
    if (err != ESP_OK) {
        char text[NODE_STATUS_BYTES];
        (void)snprintf(text, sizeof(text), "recovery pulses failed: %s", esp_err_to_name(err));
        set_status(NODE_FAILED, 0u, 0u, text);
        ESP_LOGE(TAG, "%s", text);
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(RECOVERY_BOOT_MS));
    can_flash_host_info_t info;
    const can_flash_host_status_t status = can_flash_host_info(&s_host, s_cmd_id, s_reply_id, &info);
    char text[NODE_STATUS_BYTES];
    if (status == CAN_FLASH_HOST_STATUS_OK) {
        /* Provisioned names are ASCII, but keep quotes/control bytes from
         * breaking the status JSON if a node sends malformed INFO data. */
        char name[DFU_CONFIG_STR_MAX + 1u];
        for (uint32_t i = 0u; i < info.name_len; ++i) {
            const unsigned char c = (unsigned char)info.name[i];
            name[i] = c >= 0x20u && c <= 0x7Eu && c != '"' && c != '\\' ? (char)c : '?';
        }
        name[info.name_len] = '\0';
        (void)snprintf(text, sizeof(text), "recovered: node answers INFO (name \"%s\", protocol %u)",
                       name, (unsigned)info.protocol_version);
        set_status(NODE_SUCCESS, 0u, 0u, text);
    } else {
        char diagnostic[NODE_STATUS_BYTES / 2u];
        can_flash_host_result_text(&s_host.result, diagnostic, sizeof(diagnostic));
        (void)snprintf(text, sizeof(text), "no INFO after recovery pulses (%s)", diagnostic);
        set_status(NODE_FAILED, 0u, 0u, text);
    }
    ESP_LOGI(TAG, "node recovery cmd 0x%03X reply 0x%03X: %s", s_cmd_id, s_reply_id, text);
}

/* JSON string body: printable ASCII except quote/backslash as is, every other
 * byte as a JSON unicode escape (non-ASCII as U+FFFD, like scan names). */
static void json_escape(const char *text, char *out, size_t capacity)
{
    size_t written = 0u;
    for (; *text != '\0'; ++text) {
        const uint8_t c = (uint8_t)*text;
        const bool plain = c >= 0x20u && c < 0x7Fu && c != '"' && c != '\\';
        const size_t needed = plain ? 1u : JSON_ESCAPE_BYTES;
        if (written + needed >= capacity) { break; }
        if (plain) {
            out[written] = (char)c;
        } else {
            (void)snprintf(&out[written], capacity - written, "\\u%04x", c < 0x80u ? c : 0xFFFDu);
        }
        written += needed;
    }
    out[written] = '\0';
}

esp_err_t node_flash_app_info(httpd_req_t *req)
{
    if (!s_link_ready) { return respond(req, "503 Service Unavailable", "CAN link unavailable\n"); }
    uint16_t cmd_id = CAN_ID_HOST_TO_BOOT, reply_id = CAN_ID_BOOT_TO_HOST;
    if (!get_ids(req, "cmd", &cmd_id, "reply", &reply_id)) {
        return respond(req, "400 Bad Request", "cmd/reply must be standard CAN IDs (0..0x7FF)\n");
    }
    app_control_info_t info;
    const can_flash_host_status_t status = can_flash_host_app_info(&s_host, cmd_id, reply_id, &info);
    char json[APP_JSON_BYTES];
    if (status == CAN_FLASH_HOST_STATUS_OK) {
        char project[APP_TEXT_BYTES], version[APP_TEXT_BYTES];
        json_escape(info.project_name, project, sizeof(project));
        json_escape(info.version, version, sizeof(version));
        (void)snprintf(json, sizeof(json), "{\"cmd_id\":%u,\"reply_id\":%u,\"mode\":\"app\",\"protocol\":%u,"
                       "\"project\":\"%s\",\"version\":\"%s\"}\n", cmd_id, reply_id,
                       (unsigned)info.protocol_version, project, version);
    } else if (status == CAN_FLASH_HOST_STATUS_IN_UPDATER) {
        (void)snprintf(json, sizeof(json), "{\"cmd_id\":%u,\"reply_id\":%u,\"mode\":\"updater\"}\n",
                       cmd_id, reply_id);
    } else {
        char text[NODE_STATUS_BYTES];
        can_flash_host_result_text(&s_host.result, text, sizeof(text));
        ESP_LOGI(TAG, "APP_INFO cmd 0x%03X reply 0x%03X: %s", cmd_id, reply_id, text);
        return respond(req, status == CAN_FLASH_HOST_STATUS_TIMEOUT ? "504 Gateway Timeout" : "502 Bad Gateway", text);
    }
    esp_err_t err = httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK) { err = httpd_resp_sendstr(req, json); }
    return err;
}

esp_err_t node_flash_status(httpd_req_t *req)
{
    static const char *const names[] = { "idle", "staging", "flashing", "recovering", "success", "failed" };
    node_status_t status;
    portENTER_CRITICAL(&s_status_lock);
    status = s_status;
    portEXIT_CRITICAL(&s_status_lock);
    char json[NODE_JSON_BYTES];
    /* text is exclusively internal fixed/protocol diagnostic text, never an
     * HTTP input or device name, so it cannot inject JSON quotes. */
    (void)snprintf(json, sizeof(json), "{\"state\":\"%s\",\"percent\":%" PRIu32 ",\"bytes\":%" PRIu32
                   ",\"total\":%" PRIu32 ",\"status\":\"%s\"}\n", names[status.state],
                   status.total == 0u ? 0u : status.bytes * PERCENT_MAX / status.total,
                   status.bytes, status.total, status.text);
    esp_err_t err = httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK) { err = httpd_resp_sendstr(req, json); }
    return err;
}
