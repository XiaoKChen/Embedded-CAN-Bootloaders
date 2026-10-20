/**
 * @file    package_update.c
 * @brief   Brain + nodes update packages (see package_update.h for the contract).
 *
 * Implementation notes: the upload is erased, written and read back into pkg chunk by chunk, then
 * the manifest, every payload SHA-256 and each image envelope are checked before 202. A changed
 * brain image is written and verified into the inactive slot, the pending record saved, and the
 * brain restarts; after boot, resume() checks the running image against the record before
 * relaying. Per node: ENTER_UPDATER over CAN, then INFO, then the GPIO3 pulse train if nothing
 * answered. NVS is never erased to hide an initialization failure. Uploads and resumed
 * packages obey the shared IDU limit even on the larger legacy brain partition.
 *
 * @see     package_update.h, update_pkg.h, node_flash.h
 */

#include "package_update.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "node_flash.h"
#include "node_reset.h"
#include "update_pkg.h"

#define PKG_LABEL "pkg"
#define PKG_SUBTYPE ((esp_partition_subtype_t)0x43)
#define CHUNK_SIZE 4096u
#define TEXT_SIZE 192u
#define ESCAPED_SIZE (TEXT_SIZE * 6u + 1u)
#define JSON_SIZE (ESCAPED_SIZE + UPDATE_PKG_NAME_MAX * 6u + 256u)
#define BOOT_WAIT_MS 1000u
#define RECORD_VERSION 1u
#define NVS_NAMESPACE "package"
#define NVS_KEY "pending"
#define PERCENT_MAX 100u

static const char *TAG = "package";
static const esp_partition_t *s_partition;
static nvs_handle_t s_nvs;
static bool s_storage_ready;
static bool s_blocked;
static uint8_t s_chunk[CHUNK_SIZE], s_readback[CHUNK_SIZE];
static uint8_t s_manifest[UPDATE_PKG_MANIFEST_MAX];
static update_pkg_t s_pkg;
static can_flash_host_t s_host;
static const update_pkg_entry_t *s_source;
static uint32_t s_node_index;

typedef enum { PKG_IDLE, PKG_RECEIVING, PKG_VALIDATING, PKG_UPDATING_BRAIN,
               PKG_RELAYING, PKG_DONE, PKG_FAILED } pkg_state_t;
typedef enum { BRAIN_NONE, BRAIN_SAME, BRAIN_UPDATED, BRAIN_ROLLED_BACK } brain_state_t;
/* How a node reached its updater; named in its result text (s_entry_paths). */
typedef enum { ENTRY_PARKED, ENTRY_CAN, ENTRY_PULSES } entry_path_t;
static const char *const s_entry_paths[] = {
    "updater already parked", "entered updater via CAN", "recovered via GPIO3 pulses",
};
typedef enum { NODE_PENDING, NODE_PROBING, NODE_RECOVERING, NODE_FLASHING,
               NODE_OK, NODE_FAILED, NODE_SKIPPED } relay_state_t;
typedef struct {
    uint32_t index;
    uint16_t cmd_id, reply_id;
    char name[UPDATE_PKG_NAME_MAX + 1u];
    relay_state_t state;
    uint32_t percent;
    char result[TEXT_SIZE];
} relay_status_t;
typedef struct {
    pkg_state_t state;
    brain_state_t brain;
    uint32_t bytes, total, node_count;
    char text[TEXT_SIZE];
    relay_status_t nodes[UPDATE_PKG_MAX_NODES];
} pkg_status_t;
/* Versioned fixed-width blob; ESP32-S3 is little endian. No pointers or bools.
 * Commit precedes boot selection / the first CAN mutation. */
typedef struct {
    uint32_t version, total, brain_size, brain_outcome;
    uint8_t manifest_sha[UPDATE_PKG_SHA_SIZE];
    uint8_t expected_brain_sha[UPDATE_PKG_SHA_SIZE];
} pending_record_t;
_Static_assert(sizeof(pending_record_t) == 80u, "pending record layout changed");
static pending_record_t s_record;
static pkg_status_t s_status;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static void status_set(pkg_state_t state, const char *text)
{
    portENTER_CRITICAL(&s_lock);
    s_status.state = state;
    (void)snprintf(s_status.text, sizeof(s_status.text), "%s", text);
    portEXIT_CRITICAL(&s_lock);
}

static void blocked_set(bool blocked)
{
    portENTER_CRITICAL(&s_lock);
    s_blocked = blocked;
    portEXIT_CRITICAL(&s_lock);
}

bool package_update_blocked(void)
{
    portENTER_CRITICAL(&s_lock);
    const bool blocked = s_blocked;
    portEXIT_CRITICAL(&s_lock);
    return blocked;
}

static void brain_set(brain_state_t state)
{
    portENTER_CRITICAL(&s_lock);
    s_status.brain = state;
    portEXIT_CRITICAL(&s_lock);
}

static void nodes_init(void)
{
    portENTER_CRITICAL(&s_lock);
    s_status.node_count = 0u;
    memset(s_status.nodes, 0, sizeof(s_status.nodes));
    for (uint16_t i = 0u; i < s_pkg.count; ++i) {
        const update_pkg_entry_t *e = &s_pkg.entries[i];
        if (e->type != UPDATE_PKG_NODE) { continue; }
        relay_status_t *node = &s_status.nodes[s_status.node_count++];
        node->index = i;
        node->cmd_id = e->cmd_id;
        node->reply_id = e->reply_id;
        memcpy(node->name, e->name, sizeof(node->name));
    }
    portEXIT_CRITICAL(&s_lock);
}

static void node_set(relay_state_t state, uint32_t percent, const char *result)
{
    portENTER_CRITICAL(&s_lock);
    relay_status_t *node = &s_status.nodes[s_node_index];
    node->state = state;
    node->percent = percent;
    (void)snprintf(node->result, sizeof(node->result), "%s", result);
    portEXIT_CRITICAL(&s_lock);
}

static esp_err_t respond(httpd_req_t *req, const char *status, const char *text)
{
    esp_err_t err = httpd_resp_set_status(req, status);
    if (err == ESP_OK) { err = httpd_resp_set_type(req, "text/plain"); }
    if (err == ESP_OK) { err = httpd_resp_sendstr(req, text); }
    return err;
}

static esp_err_t reject(httpd_req_t *req, const char *status, const char *text)
{
    status_set(PKG_FAILED, text);
    return respond(req, status, text);
}

static bool hash_range(const esp_partition_t *part, uint32_t offset, uint32_t size, uint8_t sha[UPDATE_PKG_SHA_SIZE])
{
    if (part == NULL || offset > part->size || size > part->size - offset) { return false; }
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    bool ok = mbedtls_sha256_starts(&ctx, 0) == 0;
    for (uint32_t done = 0u; ok && done < size;) {
        const uint32_t count = size - done < sizeof(s_chunk) ? size - done : sizeof(s_chunk);
        ok = esp_partition_read(part, offset + done, s_chunk, count) == ESP_OK &&
             mbedtls_sha256_update(&ctx, s_chunk, count) == 0;
        done += count;
        /* Bound watchdog starvation even when every flash read is cached. */
        vTaskDelay(1u);
    }
    if (ok) { ok = mbedtls_sha256_finish(&ctx, sha) == 0; }
    mbedtls_sha256_free(&ctx);
    return ok;
}

static bool running_hash(uint32_t size, uint8_t sha[UPDATE_PKG_SHA_SIZE])
{
    /* esp_partition_get_sha256 returns the appended digest, excluding the
     * digest itself. Package hashes include EVERY byte of the .bin instead. */
    return hash_range(esp_ota_get_running_partition(), 0u, size, sha);
}

static bool running_size(uint32_t *size)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running == NULL) { return false; }
    const esp_partition_pos_t pos = { .offset = running->address, .size = running->size };
    esp_image_metadata_t metadata;
    if (esp_image_get_metadata(&pos, &metadata) != ESP_OK) { return false; }
    *size = metadata.image_len;
    return *size != 0u && *size <= running->size;
}

static bool image_header_valid(const update_pkg_entry_t *entry)
{
    esp_image_header_t header;
    uint32_t app_magic;
    const uint32_t desc_offset = sizeof(header) + sizeof(esp_image_segment_header_t);
    if (entry->size < desc_offset + sizeof(app_magic) ||
        esp_partition_read(s_partition, entry->offset, &header, sizeof(header)) != ESP_OK ||
        header.magic != ESP_IMAGE_HEADER_MAGIC || header.chip_id != ESP_CHIP_ID_ESP32S3 ||
        header.segment_count == 0u || header.segment_count > ESP_IMAGE_MAX_SEGMENTS || header.hash_appended > 1u ||
        esp_partition_read(s_partition, entry->offset + desc_offset, &app_magic, sizeof(app_magic)) != ESP_OK ||
        app_magic != ESP_APP_DESC_MAGIC_WORD) { return false; }
    /* The SDK verifier requires physical 64 KiB segment alignment, so cannot
     * verify arbitrary four-byte-aligned pkg offsets. Bounds-check the image
     * envelope here; esp_ota_end verifies the real destination before GO/boot.
     * Canonical length also makes reboot hash comparisons unambiguous. */
    uint32_t offset = sizeof(header);
    for (uint8_t i = 0u; i < header.segment_count; ++i) {
        esp_image_segment_header_t segment;
        if (offset > entry->size || sizeof(segment) > entry->size - offset ||
            esp_partition_read(s_partition, entry->offset + offset, &segment, sizeof(segment)) != ESP_OK) { return false; }
        offset += sizeof(segment);
        if (segment.data_len % UPDATE_PKG_ALIGNMENT != 0u || segment.data_len > entry->size - offset) { return false; }
        offset += segment.data_len;
    }
    const uint32_t checksum_end = (offset + 16u) & ~15u;
    return checksum_end + (header.hash_appended ? UPDATE_PKG_SHA_SIZE : 0u) == entry->size;
}

static const char *load_manifest(uint32_t total, const uint8_t *expected_manifest)
{
    if (s_partition == NULL || total < UPDATE_PKG_HEADER_SIZE ||
        total > UPDATE_PKG_PARTITION_SIZE || total > s_partition->size) {
        return "Package size is outside pkg partition";
    }
    const uint32_t count = total < sizeof(s_manifest) ? total : sizeof(s_manifest);
    if (esp_partition_read(s_partition, 0u, s_manifest, count) != ESP_OK) { return "Manifest flash read failed"; }
    const update_pkg_status_t status = update_pkg_parse(s_manifest, count, total, &s_pkg);
    if (status != UPDATE_PKG_OK) { return update_pkg_status_text(status); }
    const uint32_t manifest_size = UPDATE_PKG_HEADER_SIZE + s_pkg.count * UPDATE_PKG_ENTRY_SIZE;
    uint8_t sha[UPDATE_PKG_SHA_SIZE];
    if (mbedtls_sha256(s_manifest, manifest_size, sha, 0) != 0) { return "Manifest SHA-256 failed"; }
    if (expected_manifest != NULL && memcmp(sha, expected_manifest, sizeof(sha)) != 0) {
        return "Pending package identity differs from flash; relay aborted";
    }
    nodes_init();
    return NULL;
}

static const char *validate(uint32_t total, const uint8_t *expected_manifest)
{
    status_set(PKG_VALIDATING, "Validating manifest and every payload from flash");
    const char *failure = load_manifest(total, expected_manifest);
    if (failure != NULL) { return failure; }
    uint8_t sha[UPDATE_PKG_SHA_SIZE];
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    for (uint16_t i = 0u; i < s_pkg.count; ++i) {
        const update_pkg_entry_t *e = &s_pkg.entries[i];
        if ((e->type == UPDATE_PKG_NODE && e->size > UPDATE_PKG_NODE_SLOT_SIZE) ||
            (e->type == UPDATE_PKG_BRAIN && (next == NULL || e->size > next->size))) {
            return "Payload exceeds its destination OTA slot";
        }
        if (!hash_range(s_partition, e->offset, e->size, sha) || memcmp(sha, e->sha256, sizeof(sha)) != 0) {
            return "Payload SHA-256 mismatch or flash read failed";
        }
        if (!image_header_valid(e)) { return "Payload is not a canonical unsigned ESP32-S3 application image"; }
    }
    return NULL;
}

static bool clear_record(void)
{
    esp_err_t err = nvs_erase_key(s_nvs, NVS_KEY);
    if (err == ESP_ERR_NVS_NOT_FOUND) { err = ESP_OK; }
    if (err == ESP_OK) { err = nvs_commit(s_nvs); }
    if (err != ESP_OK) {
        blocked_set(true);
        status_set(PKG_FAILED, "Cannot clear pending relay in NVS; new jobs blocked until reboot");
        ESP_LOGE(TAG, "clear pending: %s", esp_err_to_name(err));
        return false;
    }
    blocked_set(false);
    return true;
}

static bool save_record(void)
{
    /* A failed commit can have persisted the key. Fail closed until a reboot
     * can read it; never overwrite pkg while its identity might be retained. */
    blocked_set(true);
    esp_err_t err = nvs_set_blob(s_nvs, NVS_KEY, &s_record, sizeof(s_record));
    if (err == ESP_OK) { err = nvs_commit(s_nvs); }
    if (err != ESP_OK) {
        status_set(PKG_FAILED, "Cannot persist pending relay; new jobs blocked until reboot");
        ESP_LOGE(TAG, "save pending: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

static bool read_payload(void *user, uint32_t offset, uint8_t *buf, uint32_t length)
{
    (void)user;
    return s_source != NULL && offset <= s_source->size && length <= s_source->size - offset &&
        esp_partition_read(s_partition, s_source->offset + offset, buf, length) == ESP_OK;
}

static void progress(void *user, uint32_t bytes, uint32_t total)
{
    (void)user;
    node_set(NODE_FLASHING, total == 0u ? 0u : bytes * PERCENT_MAX / total,
             "CAN transfer; awaiting VERIFY and GO");
}

static bool no_answer(can_flash_host_status_t status)
{
    return status == CAN_FLASH_HOST_STATUS_TIMEOUT || status == CAN_FLASH_HOST_STATUS_TRANSPORT;
}

/* Per node: ENTER_UPDATER over CAN (+ boot wait on an app ACK) and INFO; if
 * nothing answered, the GPIO3 pulse train and INFO. Each result names the
 * path that reached the updater (entry_path_t). */
static void relay(void)
{
    status_set(PKG_RELAYING, "Relaying node entries in manifest order");
    const bool transport = node_flash_transport(&s_host);
    s_host.read = read_payload;
    s_host.progress = progress;
    bool failed = false;
    s_node_index = 0u;
    for (uint16_t i = 0u; i < s_pkg.count; ++i) {
        const update_pkg_entry_t *e = &s_pkg.entries[i];
        if (e->type != UPDATE_PKG_NODE) { continue; }
        s_source = e;
        if (!transport) {
            node_set(NODE_FAILED, 0u, "CAN link unavailable");
            failed = true;
            ++s_node_index;
            continue;
        }
        /* ENTER_UPDATER first: it answers fast in every case (an app ACKs and
         * parks itself; the updater NAKs it, i.e. already parked), so only a
         * node with no CAN-aware app pays for the slow INFO timeout. */
        node_set(NODE_PROBING, 0u, "Sending ENTER_UPDATER over CAN");
        const can_flash_host_status_t entry = node_flash_enter_updater(&s_host, e->cmd_id, e->reply_id);
        entry_path_t path = entry == CAN_FLASH_HOST_STATUS_OK ? ENTRY_CAN : ENTRY_PARKED;
        can_flash_host_info_t info;
        can_flash_host_status_t result = CAN_FLASH_HOST_STATUS_TIMEOUT;
        if (entry == CAN_FLASH_HOST_STATUS_OK || entry == CAN_FLASH_HOST_STATUS_IN_UPDATER) {
            node_set(NODE_PROBING, 0u, "INFO probe");
            result = can_flash_host_info(&s_host, e->cmd_id, e->reply_id, &info);
        }
        /* No CAN entry (no app CAN service, or no updater after the ACK): the
         * electrical way. */
        if (no_answer(result)) {
            node_set(NODE_RECOVERING, 0u, "No CAN entry; pulsing GPIO3 (only the wired node can recover)");
            const esp_err_t err = node_reset_pulse_train(NODE_RESET_RECOVERY_PULSES,
                NODE_RESET_RECOVERY_HIGH_MS, NODE_RESET_RECOVERY_LOW_MS);
            if (err != ESP_OK) {
                char text[TEXT_SIZE];
                (void)snprintf(text, sizeof(text), "recovery pulses failed: %s", esp_err_to_name(err));
                node_set(NODE_FAILED, 0u, text);
                failed = true;
                ++s_node_index;
                continue;
            }
            vTaskDelay(pdMS_TO_TICKS(BOOT_WAIT_MS));
            path = ENTRY_PULSES;
            result = can_flash_host_info(&s_host, e->cmd_id, e->reply_id, &info);
        }
        char text[TEXT_SIZE];
        char diagnostic[TEXT_SIZE / 2u];
        if (result != CAN_FLASH_HOST_STATUS_OK) {
            can_flash_host_result_text(&s_host.result, diagnostic, sizeof(diagnostic));
            (void)snprintf(text, sizeof(text), "%s (%s)", diagnostic,
                           path == ENTRY_PULSES ? "no updater even after ENTER_UPDATER and GPIO3 pulses" :
                           s_entry_paths[path]);
            node_set(NODE_FAILED, 0u, text);
            failed = true;
        } else if (e->name_len != 0u && (e->name_len != info.name_len ||
                   memcmp(e->name, info.name, e->name_len) != 0)) {
            (void)snprintf(text, sizeof(text), "product name mismatch; node not flashed (%s)", s_entry_paths[path]);
            node_set(NODE_SKIPPED, 0u, text);
            failed = true;
        } else {
            (void)snprintf(text, sizeof(text), "CAN handshake (%s)", s_entry_paths[path]);
            node_set(NODE_FLASHING, 0u, text);
            can_flash_host_result_t flash_result;
            result = can_flash_host_flash(&s_host, e->cmd_id, e->reply_id, e->size, &flash_result);
            const bool ok = result == CAN_FLASH_HOST_STATUS_OK;
            if (ok) {
                (void)snprintf(text, sizeof(text), "ok; %s", s_entry_paths[path]);
            } else {
                can_flash_host_result_text(&flash_result, diagnostic, sizeof(diagnostic));
                (void)snprintf(text, sizeof(text), "%s (%s)", diagnostic, s_entry_paths[path]);
            }
            node_set(ok ? NODE_OK : NODE_FAILED, ok ? PERCENT_MAX : flash_result.bytes * PERCENT_MAX / e->size, text);
            failed |= !ok;
        }
        ++s_node_index;
    }
    s_source = NULL;
    if (clear_record()) {
        status_set(PKG_DONE, failed ? "Relay finished with node failures; inspect results" : "Package complete");
    }
}

static bool write_brain(const update_pkg_entry_t *entry, const esp_partition_t *target)
{
    esp_ota_handle_t handle;
    esp_err_t err = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (err != ESP_OK) { status_set(PKG_FAILED, "Cannot begin brain OTA"); return false; }
    for (uint32_t offset = 0u; offset < entry->size;) {
        const uint32_t count = entry->size - offset < sizeof(s_chunk) ? entry->size - offset : sizeof(s_chunk);
        err = esp_partition_read(s_partition, entry->offset + offset, s_chunk, count);
        if (err == ESP_OK) { err = esp_ota_write(handle, s_chunk, count); }
        if (err != ESP_OK) {
            const esp_err_t abort_err = esp_ota_abort(handle);
            if (abort_err != ESP_OK) { ESP_LOGE(TAG, "abort OTA: %s", esp_err_to_name(abort_err)); }
            status_set(PKG_FAILED, "Brain OTA read/write failed; boot partition unchanged");
            return false;
        }
        offset += count;
    }
    err = esp_ota_end(handle);
    uint8_t sha[UPDATE_PKG_SHA_SIZE];
    if (err != ESP_OK || !hash_range(target, 0u, entry->size, sha) ||
        memcmp(sha, entry->sha256, sizeof(sha)) != 0) {
        status_set(PKG_FAILED, "Brain image invalid or OTA read-back SHA mismatch; node relay aborted");
        return false;
    }
    return true;
}

void package_update_run(void)
{
    const update_pkg_entry_t *brain = NULL;
    for (uint16_t i = 0u; i < s_pkg.count; ++i) {
        if (s_pkg.entries[i].type == UPDATE_PKG_BRAIN) { brain = &s_pkg.entries[i]; }
    }
    uint32_t size;
    uint8_t sha[UPDATE_PKG_SHA_SIZE];
    if (!running_size(&size) || !running_hash(size, sha)) {
        status_set(PKG_FAILED, "Cannot hash the running brain image; relay aborted");
        return;
    }
    const bool changed = brain != NULL && (brain->size != size || memcmp(sha, brain->sha256, sizeof(sha)) != 0);
    const brain_state_t outcome = brain == NULL ? BRAIN_NONE : changed ? BRAIN_UPDATED : BRAIN_SAME;
    brain_set(outcome);
    s_record = (pending_record_t){ .version = RECORD_VERSION, .total = s_pkg.total_size,
        .brain_size = changed ? brain->size : size, .brain_outcome = outcome };
    memcpy(s_record.expected_brain_sha, changed ? brain->sha256 : sha, sizeof(sha));
    const uint32_t manifest_size = UPDATE_PKG_HEADER_SIZE + s_pkg.count * UPDATE_PKG_ENTRY_SIZE;
    if (mbedtls_sha256(s_manifest, manifest_size, s_record.manifest_sha, 0) != 0) {
        status_set(PKG_FAILED, "Cannot hash manifest; relay aborted");
        return;
    }
    if (changed) {
        status_set(PKG_UPDATING_BRAIN, "Writing and verifying inactive brain OTA slot");
        const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
        if (target == NULL || !write_brain(brain, target)) { return; }
        if (!save_record()) { return; }
        if (esp_ota_set_boot_partition(target) != ESP_OK) {
            if (clear_record()) { status_set(PKG_FAILED, "Cannot select brain OTA slot; node relay aborted"); }
            return;
        }
        status_set(PKG_UPDATING_BRAIN, "Brain verified; rebooting before node relay");
        vTaskDelay(pdMS_TO_TICKS(BOOT_WAIT_MS));
        esp_restart();
    }
    if (save_record()) { relay(); }
}

void package_update_resume(void)
{
    s_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, PKG_SUBTYPE, PKG_LABEL);
    esp_err_t err = nvs_flash_init();
    /* Never erase NVS to hide an initialization failure: it may contain the
     * only pending-relay identity and the board's other persistent settings. */
    if (err == ESP_OK) { err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &s_nvs); }
    if (err != ESP_OK) {
        blocked_set(true);
        status_set(PKG_FAILED, "NVS unavailable; jobs blocked to preserve any pending relay");
        return;
    }
    s_storage_ready = true;
    size_t record_size = sizeof(s_record);
    err = nvs_get_blob(s_nvs, NVS_KEY, &s_record, &record_size);
    if (err == ESP_ERR_NVS_NOT_FOUND) { return; }
    blocked_set(true);
    if (err != ESP_OK || record_size != sizeof(s_record) || s_record.version != RECORD_VERSION ||
        s_record.brain_outcome > BRAIN_UPDATED || s_record.brain_size == 0u ||
        s_record.brain_size > UPDATE_PKG_BRAIN_SLOT_SIZE) {
        status_set(PKG_FAILED, "Unknown or damaged pending record; jobs blocked (service required)");
        return;
    }
    brain_set((brain_state_t)s_record.brain_outcome);
    portENTER_CRITICAL(&s_lock);
    s_status.total = s_record.total;
    s_status.bytes = s_record.total;
    portEXIT_CRITICAL(&s_lock);
    uint8_t sha[UPDATE_PKG_SHA_SIZE];
    uint32_t size;
    if (!running_size(&size) || !running_hash(size, sha)) {
        status_set(PKG_FAILED, "Cannot verify running image; pending relay retained, jobs blocked");
        return;
    }
    if (size != s_record.brain_size || memcmp(sha, s_record.expected_brain_sha, sizeof(sha)) != 0) {
        /* Recover display-only node identities if the manifest is still safe;
         * no payload or CAN operation is needed on the rollback path. */
        if (load_manifest(s_record.total, s_record.manifest_sha) == NULL) {
            s_node_index = 0u;
            for (uint16_t i = 0u; i < s_pkg.count; ++i) {
                if (s_pkg.entries[i].type == UPDATE_PKG_NODE) {
                    node_set(NODE_SKIPPED, 0u, "brain update rolled back; node relay aborted");
                    ++s_node_index;
                }
            }
        }
        brain_set(BRAIN_ROLLED_BACK);
        if (clear_record()) { status_set(PKG_FAILED, "brain update rolled back; node relay aborted"); }
        return;
    }
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) != ESP_OK || state != ESP_OTA_IMG_VALID) {
        status_set(PKG_UPDATING_BRAIN, "Brain not confirmed VALID; relay retained but blocked until next boot");
        return;
    }
    const char *failure = validate(s_record.total, s_record.manifest_sha);
    if (failure != NULL) {
        if (clear_record()) { status_set(PKG_FAILED, failure); }
        return;
    }
    relay();
}

esp_err_t package_update_stage(httpd_req_t *req, bool *ready)
{
    *ready = false;
    if (!s_storage_ready || s_partition == NULL || s_partition->encrypted || s_partition->readonly) {
        return reject(req, "503 Service Unavailable", "Writable unencrypted pkg partition and NVS required; USB-reflash new table");
    }
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
        return reject(req, "409 Conflict", "Running brain image is not confirmed");
    }
    if (req->content_len == 0u) { return reject(req, "411 Length Required", "Nonempty Content-Length required"); }
    if (req->content_len > UPDATE_PKG_PARTITION_SIZE || req->content_len > s_partition->size) {
        return reject(req, "413 Payload Too Large", "Package exceeds supported pkg partition limit");
    }
    const uint32_t total = (uint32_t)req->content_len;
    portENTER_CRITICAL(&s_lock);
    memset(&s_status, 0, sizeof(s_status));
    s_status.total = total;
    portEXIT_CRITICAL(&s_lock);
    status_set(PKG_RECEIVING, "Receiving into pkg; active images unchanged");
    uint32_t received = 0u, erased = 0u;
    while (received < total) {
        const uint32_t left = total - received;
        const int got = httpd_req_recv(req, (char *)s_chunk, left < sizeof(s_chunk) ? left : sizeof(s_chunk));
        if (got == HTTPD_SOCK_ERR_TIMEOUT) { return reject(req, "408 Request Timeout", "Package receive timeout"); }
        if (got <= 0) { status_set(PKG_FAILED, "Package upload connection closed"); return ESP_FAIL; }
        const uint32_t end = received + (uint32_t)got;
        while (erased < end) {
            if (s_partition->erase_size == 0u || s_partition->erase_size > s_partition->size - erased ||
                esp_partition_erase_range(s_partition, erased, s_partition->erase_size) != ESP_OK) {
                return reject(req, "500 Internal Server Error", "pkg erase failed");
            }
            erased += s_partition->erase_size;
        }
        if (esp_partition_write(s_partition, received, s_chunk, (size_t)got) != ESP_OK ||
            esp_partition_read(s_partition, received, s_readback, (size_t)got) != ESP_OK ||
            memcmp(s_chunk, s_readback, (size_t)got) != 0) {
            return reject(req, "500 Internal Server Error", "pkg write/read-back failed");
        }
        received = end;
        portENTER_CRITICAL(&s_lock);
        s_status.bytes = received;
        portEXIT_CRITICAL(&s_lock);
    }
    const char *failure = validate(total, NULL);
    if (failure != NULL) { return reject(req, "422 Unprocessable Content", failure); }
    const esp_err_t err = respond(req, "202 Accepted", "Package staged and checked; poll /package/status\n");
    if (err == ESP_OK) { *ready = true; }
    else { status_set(PKG_FAILED, "Response connection closed; no brain update or relay started"); }
    return err;
}

static void escape(const char *text, char *out, size_t capacity)
{
    size_t written = 0u;
    while (*text != '\0') {
        const uint8_t c = (uint8_t)*text;
        const bool special = c == '"' || c == '\\';
        const bool printable = c >= 0x20u && c < 0x7Fu;
        const size_t needed = (printable && !special) ? 1u : (special ? 2u : 6u);
        if (written + needed >= capacity) { break; }  /* keep room for the NUL */
        if (printable && !special) {
            out[written] = (char)c;
        } else if (special) {
            out[written] = '\\';
            out[written + 1u] = (char)c;
        } else {
            (void)snprintf(out + written, capacity - written, "\\u%04x", c < 0x80u ? c : 0xFFFDu);
        }
        written += needed;
        text++;
    }
    out[written] = '\0';
}

esp_err_t package_update_status(httpd_req_t *req)
{
    static const char *const states[] = { "idle", "receiving", "validating", "updating_brain", "relaying", "done", "failed" };
    static const char *const brains[] = { "none", "skipped_same", "updated", "rolled_back" };
    static const char *const nodes[] = { "pending", "probing", "recovering", "flashing", "ok", "failed", "skipped" };
    pkg_status_t status;
    portENTER_CRITICAL(&s_lock);
    status = s_status;
    portEXIT_CRITICAL(&s_lock);
    char text[ESCAPED_SIZE], json[JSON_SIZE];
    escape(status.text, text, sizeof(text));
    (void)snprintf(json, sizeof(json), "{\"state\":\"%s\",\"status\":\"%s\",\"bytes\":%" PRIu32
        ",\"total\":%" PRIu32 ",\"brain\":\"%s\",\"nodes\":[", states[status.state], text,
        status.bytes, status.total, brains[status.brain]);
    esp_err_t err = httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK) { err = httpd_resp_sendstr_chunk(req, json); }
    for (uint32_t i = 0u; i < status.node_count && err == ESP_OK; ++i) {
        const relay_status_t *node = &status.nodes[i];
        char name[UPDATE_PKG_NAME_MAX * 6u + 1u];
        escape(node->name, name, sizeof(name));
        escape(node->result, text, sizeof(text));
        (void)snprintf(json, sizeof(json), "%s{\"index\":%" PRIu32 ",\"cmd_id\":%u,\"reply_id\":%u,"
            "\"name\":\"%s\",\"state\":\"%s\",\"percent\":%" PRIu32 ",\"result\":\"%s\"}",
            i == 0u ? "" : ",", node->index, node->cmd_id, node->reply_id, name,
            nodes[node->state], node->percent, text);
        err = httpd_resp_sendstr_chunk(req, json);
    }
    if (err == ESP_OK) { err = httpd_resp_sendstr_chunk(req, "]}\n"); }
    if (err == ESP_OK) { err = httpd_resp_sendstr_chunk(req, NULL); }
    return err;
}
