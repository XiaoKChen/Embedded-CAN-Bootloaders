/* =============================================================================
 * can_flash_host.c — Blocking host protocol over injected bus and image callbacks
 * ============================================================================= */
#include "can_flash_host.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "crc32.h"

static const uint16_t s_known_cmd_ids[] = { 0x01u, 0x03u, 0x05u, 0x07u, 0x09u };

static uint32_t minimum(uint32_t a, uint32_t b) { return a < b ? a : b; }

static bool valid_host(const can_flash_host_t *h)
{
    return h != NULL && h->send != NULL && h->receive != NULL && h->clock_ms != NULL;
}

static can_flash_host_status_t outcome(can_flash_host_t *h, can_flash_host_status_t status)
{
    h->result.status = status;
    return status;
}

static uint32_t remaining(can_flash_host_t *h, uint32_t start, uint32_t timeout)
{
    const uint32_t elapsed = h->clock_ms(h->user) - start;
    return elapsed < timeout ? timeout - elapsed : 0u;
}

static bool data_frame(const can_flash_host_frame_t *f)
{
    return !f->extended && !f->remote && !f->error && f->id <= DFU_CONFIG_CAN_ID_MAX &&
           f->dlc == CAN_FLASH_HOST_DLC;
}

static void payload(uint8_t p[CAN_FLASH_HOST_DLC], uint8_t cmd, uint32_t value)
{
    memset(p, CAN_UPDATE_FILL_BYTE, CAN_FLASH_HOST_DLC);
    p[0] = cmd;
    if (cmd == CAN_UPDATE_CMD_HELLO) {
        p[1] = CAN_UPDATE_PROTOCOL_VERSION;
    } else if (cmd == CAN_UPDATE_CMD_SIZE || cmd == CAN_UPDATE_CMD_VERIFY) {
        for (uint32_t i = 0u; i < sizeof(value); ++i) {
            p[2u + i] = (uint8_t)(value >> (i * 8u));
        }
    }
}

static can_flash_host_status_t send_frame(can_flash_host_t *h, uint16_t id, const uint8_t *p)
{
    can_flash_host_frame_t f = { .id = id, .dlc = CAN_FLASH_HOST_DLC };
    memcpy(f.data, p, sizeof(f.data));
    const can_flash_host_io_t io = h->send(h->user, &f, CAN_FLASH_HOST_SEND_MS);
    return io == CAN_FLASH_HOST_IO_OK ? CAN_FLASH_HOST_STATUS_OK :
           outcome(h, CAN_FLASH_HOST_STATUS_TRANSPORT);
}

static can_flash_host_status_t drain(can_flash_host_t *h, uint32_t quiet)
{
    can_flash_host_frame_t f;
    /* Also bound a continuously busy/foreign bus, rather than hang forever. */
    const uint32_t start = h->clock_ms(h->user);
    for (;;) {
        const can_flash_host_io_t io = h->receive(h->user, &f, quiet);
        if (io == CAN_FLASH_HOST_IO_TIMEOUT) { return CAN_FLASH_HOST_STATUS_OK; }
        if (io != CAN_FLASH_HOST_IO_OK) { return outcome(h, CAN_FLASH_HOST_STATUS_TRANSPORT); }
        if (remaining(h, start, CAN_FLASH_HOST_ACK_MS) == 0u) {
            return outcome(h, CAN_FLASH_HOST_STATUS_TIMEOUT);
        }
    }
}

static can_flash_host_status_t wait_reply(can_flash_host_t *h, uint16_t reply_id,
                                          bool any_id, uint8_t cmd, uint32_t timeout,
                                          bool poll, can_flash_host_frame_t *f)
{
    const uint32_t start = h->clock_ms(h->user);
    do {
        const can_flash_host_io_t io = h->receive(h->user, f, remaining(h, start, timeout));
        if (io == CAN_FLASH_HOST_IO_TIMEOUT) { return CAN_FLASH_HOST_STATUS_TIMEOUT; }
        if (io != CAN_FLASH_HOST_IO_OK) { return outcome(h, CAN_FLASH_HOST_STATUS_TRANSPORT); }
        if (!any_id && f->id == reply_id && !f->extended && !f->remote && !f->error &&
            f->dlc != CAN_FLASH_HOST_DLC && !poll) {
            return outcome(h, CAN_FLASH_HOST_STATUS_PROTOCOL);
        }
        if (data_frame(f) && (any_id || f->id == reply_id) &&
            f->data[0] == CAN_UPDATE_REPLY_MARKER && f->data[1] == cmd) {
            return CAN_FLASH_HOST_STATUS_OK;
        }
    } while (!poll && remaining(h, start, timeout) != 0u);
    return CAN_FLASH_HOST_STATUS_TIMEOUT;
}

static can_flash_host_status_t command(can_flash_host_t *h, uint16_t cmd_id, uint16_t reply_id,
                                       uint8_t cmd, uint32_t value, can_flash_host_frame_t *reply)
{
    uint8_t p[CAN_FLASH_HOST_DLC];
    payload(p, cmd, value);
    h->result.step = (can_flash_host_step_t)cmd;
    h->result.nak = 0u;
    h->result.arg = 0u;
    for (uint32_t attempt = 1u; attempt <= CAN_FLASH_HOST_ATTEMPTS; ++attempt) {
        h->result.attempts = attempt;
        if (send_frame(h, cmd_id, p) != CAN_FLASH_HOST_STATUS_OK) { return h->result.status; }
        const can_flash_host_status_t status = wait_reply(h, reply_id, false, cmd,
            cmd == CAN_UPDATE_CMD_ERASE ? CAN_FLASH_HOST_ERASE_MS : CAN_FLASH_HOST_ACK_MS,
            false, reply);
        if (status == CAN_FLASH_HOST_STATUS_TIMEOUT) { continue; }
        if (status != CAN_FLASH_HOST_STATUS_OK) { return outcome(h, status); }
        h->result.nak = reply->data[2];
        h->result.arg = reply->data[3];
        return outcome(h, h->result.nak == CAN_UPDATE_OK ? CAN_FLASH_HOST_STATUS_OK : CAN_FLASH_HOST_STATUS_NAK);
    }
    return outcome(h, CAN_FLASH_HOST_STATUS_TIMEOUT);
}

static can_flash_host_status_t info_probe(can_flash_host_t *h, uint16_t cmd_id,
                                          uint16_t reply_id, bool scan, can_flash_host_info_t *info)
{
    uint8_t p[CAN_FLASH_HOST_DLC];
    payload(p, CAN_UPDATE_CMD_INFO, 0u);
    h->result.step = CAN_FLASH_HOST_STEP_INFO;
    const uint32_t timeout = scan ? CAN_FLASH_HOST_SCAN_MS : CAN_FLASH_HOST_ACK_MS;
    const uint32_t tries = scan ? 1u : CAN_FLASH_HOST_ATTEMPTS;
    for (uint32_t attempt = 1u; attempt <= tries; ++attempt) {
        h->result.attempts = attempt;
        if (drain(h, scan ? 0u : CAN_FLASH_HOST_QUIET_MS) != CAN_FLASH_HOST_STATUS_OK ||
            send_frame(h, cmd_id, p) != CAN_FLASH_HOST_STATUS_OK) { return h->result.status; }
        const uint32_t start = h->clock_ms(h->user);
        can_flash_host_frame_t f;
        can_flash_host_status_t status = wait_reply(h, reply_id, scan, CAN_UPDATE_CMD_INFO, timeout, false, &f);
        if (status == CAN_FLASH_HOST_STATUS_TIMEOUT) { continue; }
        if (status != CAN_FLASH_HOST_STATUS_OK) { return outcome(h, status); }
        can_flash_host_info_t found = {
            .cmd_id = cmd_id, .reply_id = (uint16_t)f.id,
            .name_len = f.data[2], .protocol_version = f.data[3],
        };
        uint32_t off;
        for (off = 0u; off < DFU_CONFIG_STR_MAX; off += CAN_FLASH_HOST_DLC) {
            const can_flash_host_io_t io = h->receive(h->user, &f, scan ? remaining(h, start, timeout) : timeout);
            if (io == CAN_FLASH_HOST_IO_ERROR) { return outcome(h, CAN_FLASH_HOST_STATUS_TRANSPORT); }
            if (io != CAN_FLASH_HOST_IO_OK || !data_frame(&f) || f.id != found.reply_id) { break; }
            memcpy(&found.name[off], f.data, CAN_FLASH_HOST_DLC);
        }
        if (off != DFU_CONFIG_STR_MAX || found.name_len > DFU_CONFIG_STR_MAX) { continue; }
        found.name[found.name_len] = '\0';
        *info = found;
        return outcome(h, CAN_FLASH_HOST_STATUS_OK);
    }
    return outcome(h, CAN_FLASH_HOST_STATUS_TIMEOUT);
}

can_flash_host_status_t can_flash_host_info(can_flash_host_t *h, uint16_t cmd_id,
                                           uint16_t reply_id, can_flash_host_info_t *info)
{
    if (!valid_host(h) || info == NULL || cmd_id > DFU_CONFIG_CAN_ID_MAX || reply_id > DFU_CONFIG_CAN_ID_MAX) {
        return CAN_FLASH_HOST_STATUS_INVALID_ARG;
    }
    memset(&h->result, 0, sizeof(h->result));
    memset(info, 0, sizeof(*info));
    return info_probe(h, cmd_id, reply_id, false, info);
}

can_flash_host_status_t can_flash_host_scan(can_flash_host_t *h, uint16_t from, uint16_t to,
                                           can_flash_host_info_t results[], uint32_t max, uint32_t *count)
{
    if (!valid_host(h) || count == NULL || (max != 0u && results == NULL) || from > to || to > DFU_CONFIG_CAN_ID_MAX) {
        return CAN_FLASH_HOST_STATUS_INVALID_ARG;
    }
    memset(&h->result, 0, sizeof(h->result));
    *count = 0u;
    bool full = false;
    for (uint16_t id = 0u; id <= DFU_CONFIG_CAN_ID_MAX; ++id) {
        bool known = false;
        for (size_t i = 0u; i < sizeof(s_known_cmd_ids) / sizeof(s_known_cmd_ids[0]); ++i) {
            if (id == s_known_cmd_ids[i]) { known = true; break; }
        }
        if (!(id >= from && id <= to) && id != CAN_ID_HOST_TO_BOOT && !known) { continue; }
        can_flash_host_info_t info;
        const can_flash_host_status_t status = info_probe(h, id, 0u, true, &info);
        if (status == CAN_FLASH_HOST_STATUS_TIMEOUT) { continue; }
        if (status != CAN_FLASH_HOST_STATUS_OK) { return status; }
        if (*count < max) { results[(*count)++] = info; } else { full = true; }
    }
    return outcome(h, full ? CAN_FLASH_HOST_STATUS_CAPACITY : CAN_FLASH_HOST_STATUS_OK);
}

static uint8_t start_seq(uint32_t window)
{
    return (uint8_t)(window * CAN_FLASH_HOST_FRAMES_PER_WINDOW);
}

static can_flash_host_status_t send_window(can_flash_host_t *h, uint16_t cmd_id, uint16_t reply_id,
                                           uint32_t size, uint32_t window, can_flash_host_frame_t *reply)
{
    const uint32_t offset = window * CAN_UPDATE_WINDOW_SIZE;
    const uint32_t len = minimum(CAN_UPDATE_WINDOW_SIZE, size - offset);
    if (!h->read(h->user, offset, h->buffer, len)) { return outcome(h, CAN_FLASH_HOST_STATUS_READ); }
    uint32_t frame = 0u;
    for (uint32_t off = 0u; off < len; off += CAN_UPDATE_DATA_BYTES_PER_FRAME, ++frame) {
        uint8_t p[CAN_FLASH_HOST_DLC];
        payload(p, CAN_UPDATE_CMD_DATA, 0u);
        p[1] = (uint8_t)(start_seq(window) + frame);
        memcpy(&p[2], &h->buffer[off], minimum(CAN_UPDATE_DATA_BYTES_PER_FRAME, len - off));
        if (send_frame(h, cmd_id, p) != CAN_FLASH_HOST_STATUS_OK) { return h->result.status; }
        const can_flash_host_status_t status = wait_reply(h, reply_id, false, CAN_UPDATE_CMD_DATA,
            (frame + 1u) % CAN_FLASH_HOST_EARLY_FRAMES == 0u ? CAN_FLASH_HOST_EARLY_MS : 0u, true, reply);
        if (status != CAN_FLASH_HOST_STATUS_TIMEOUT) { return status; }
    }
    return wait_reply(h, reply_id, false, CAN_UPDATE_CMD_DATA, CAN_FLASH_HOST_ACK_MS, false, reply);
}

static can_flash_host_status_t transfer(can_flash_host_t *h, uint16_t cmd_id, uint16_t reply_id, uint32_t size)
{
    const uint32_t windows = (size - 1u) / CAN_UPDATE_WINDOW_SIZE + 1u;
    uint32_t window = 0u;
    uint32_t attempts = 0u;
    can_flash_host_frame_t reply;
    bool verified = false;
    while (window < windows) {
        h->result.step = CAN_FLASH_HOST_STEP_DATA;
        h->result.window = window;
        h->result.attempts = attempts;
        if (attempts == CAN_FLASH_HOST_ATTEMPTS) { return outcome(h, CAN_FLASH_HOST_STATUS_TIMEOUT); }
        h->result.attempts = ++attempts;
        can_flash_host_status_t status = send_window(h, cmd_id, reply_id, size, window, &reply);
        if (status == CAN_FLASH_HOST_STATUS_TIMEOUT && window + 1u == windows) {
            /* VERIFY is harmless in TRANSFER (STATE, no mutation). In
             * TRANSFERRED it proves all bytes arrived, including their CRC. */
            status = command(h, cmd_id, reply_id, CAN_UPDATE_CMD_VERIFY, h->result.crc32, &reply);
            if (status == CAN_FLASH_HOST_STATUS_OK) {
                verified = true;
                ++window;
                break;
            }
            if (status != CAN_FLASH_HOST_STATUS_NAK || h->result.nak != CAN_UPDATE_ERR_STATE) { return status; }
            status = CAN_FLASH_HOST_STATUS_TIMEOUT;
        }
        if (status == CAN_FLASH_HOST_STATUS_TIMEOUT) {
            if (drain(h, CAN_FLASH_HOST_QUIET_MS) != CAN_FLASH_HOST_STATUS_OK) { return h->result.status; }
            continue;
        }
        if (status != CAN_FLASH_HOST_STATUS_OK) { return outcome(h, status); }
        h->result.nak = reply.data[2];
        h->result.arg = reply.data[3];
        const uint32_t len = minimum(CAN_UPDATE_WINDOW_SIZE, size - window * CAN_UPDATE_WINDOW_SIZE);
        const uint32_t frames = (len + CAN_UPDATE_DATA_BYTES_PER_FRAME - 1u) / CAN_UPDATE_DATA_BYTES_PER_FRAME;
        if (reply.data[2] == CAN_UPDATE_OK && reply.data[3] == (uint8_t)(start_seq(window) + frames - 1u)) {
            ++window;
            attempts = 0u;
        } else {
            if (reply.data[2] != CAN_UPDATE_OK && reply.data[2] != CAN_UPDATE_ERR_SEQ && reply.data[2] != CAN_UPDATE_ERR_FLASH) {
                return outcome(h, CAN_FLASH_HOST_STATUS_NAK);
            }
            if (drain(h, CAN_FLASH_HOST_QUIET_MS) != CAN_FLASH_HOST_STATUS_OK) { return h->result.status; }
            if (reply.data[2] == CAN_UPDATE_ERR_SEQ) {
                if (window + 1u < windows && reply.data[3] == start_seq(window + 1u)) {
                    ++window;
                    attempts = 0u;
                } else if (reply.data[3] != start_seq(window)) {
                    return outcome(h, CAN_FLASH_HOST_STATUS_PROTOCOL);
                }
            }
        }
        h->result.bytes = minimum(window * CAN_UPDATE_WINDOW_SIZE, size);
        if (h->progress != NULL) { h->progress(h->user, h->result.bytes, size); }
    }
    h->result.bytes = size;
    if (h->progress != NULL) { h->progress(h->user, size, size); }
    if (!verified && command(h, cmd_id, reply_id, CAN_UPDATE_CMD_VERIFY, h->result.crc32, &reply) != CAN_FLASH_HOST_STATUS_OK) {
        return h->result.status;
    }
    return command(h, cmd_id, reply_id, CAN_UPDATE_CMD_GO, 0u, &reply);
}

can_flash_host_status_t can_flash_host_flash(can_flash_host_t *h, uint16_t cmd_id,
                                            uint16_t reply_id, uint32_t image_size, can_flash_host_result_t *result)
{
    if (result == NULL) { return CAN_FLASH_HOST_STATUS_INVALID_ARG; }
    memset(result, 0, sizeof(*result));
    if (!valid_host(h) || h->read == NULL || image_size == 0u ||
        cmd_id > DFU_CONFIG_CAN_ID_MAX || reply_id > DFU_CONFIG_CAN_ID_MAX) {
        result->status = CAN_FLASH_HOST_STATUS_INVALID_ARG;
        return result->status;
    }
    memset(&h->result, 0, sizeof(h->result));
    can_flash_host_frame_t reply;
    if (command(h, cmd_id, reply_id, CAN_UPDATE_CMD_HELLO, 0u, &reply) != CAN_FLASH_HOST_STATUS_OK) { goto done; }
    if (reply.data[3] != CAN_UPDATE_PROTOCOL_VERSION) { outcome(h, CAN_FLASH_HOST_STATUS_PROTOCOL); goto done; }
    for (uint32_t i = 0u; i < sizeof(h->result.slot_size); ++i) {
        h->result.slot_size |= (uint32_t)reply.data[4u + i] << (i * 8u);
    }
    if (command(h, cmd_id, reply_id, CAN_UPDATE_CMD_SIZE, image_size, &reply) != CAN_FLASH_HOST_STATUS_OK) { goto done; }
    /* CRC before mutation, over the immutable image, not retransmitted bytes. */
    for (uint32_t off = 0u; off < image_size;) {
        const uint32_t len = minimum(CAN_UPDATE_WINDOW_SIZE, image_size - off);
        if (!h->read(h->user, off, h->buffer, len)) { outcome(h, CAN_FLASH_HOST_STATUS_READ); goto done; }
        h->result.crc32 = crc32_ieee_update(h->result.crc32, h->buffer, len);
        off += len;
    }
    if (command(h, cmd_id, reply_id, CAN_UPDATE_CMD_ERASE, 0u, &reply) != CAN_FLASH_HOST_STATUS_OK) { goto done; }
    outcome(h, transfer(h, cmd_id, reply_id, image_size));
done:
    *result = h->result;
    return result->status;
}

/* Discard frames already queued (stale replies) without waiting for a quiet
 * bus, which a bus full of running apps may never offer. */
static can_flash_host_status_t flush_queued(can_flash_host_t *h)
{
    can_flash_host_frame_t f;
    for (uint32_t i = 0u; i < CAN_FLASH_HOST_FLUSH_FRAMES; ++i) {
        const can_flash_host_io_t io = h->receive(h->user, &f, 0u);
        if (io == CAN_FLASH_HOST_IO_TIMEOUT) { return CAN_FLASH_HOST_STATUS_OK; }
        if (io != CAN_FLASH_HOST_IO_OK) { return outcome(h, CAN_FLASH_HOST_STATUS_TRANSPORT); }
    }
    return CAN_FLASH_HOST_STATUS_OK;
}

/* Next standard data frame on reply_id before the attempt deadline. Frames on
 * other IDs belong to other traffic and are skipped. */
static can_flash_host_status_t app_frame(can_flash_host_t *h, uint16_t reply_id, uint32_t start,
                                         can_flash_host_frame_t *f)
{
    for (;;) {
        const uint32_t left = remaining(h, start, CAN_FLASH_HOST_APP_MS);
        if (left == 0u) { return CAN_FLASH_HOST_STATUS_TIMEOUT; }
        const can_flash_host_io_t io = h->receive(h->user, f, left);
        if (io == CAN_FLASH_HOST_IO_TIMEOUT) { return CAN_FLASH_HOST_STATUS_TIMEOUT; }
        if (io != CAN_FLASH_HOST_IO_OK) { return outcome(h, CAN_FLASH_HOST_STATUS_TRANSPORT); }
        if (f->id == reply_id && !f->extended && !f->remote && !f->error) {
            return f->dlc == CAN_FLASH_HOST_DLC ? CAN_FLASH_HOST_STATUS_OK : CAN_FLASH_HOST_STATUS_PROTOCOL;
        }
    }
}

/* The 8 APP_INFO body frames after an accepted header, then the codec's
 * complete-burst checks. */
static can_flash_host_status_t app_info_body(can_flash_host_t *h, uint16_t reply_id, uint32_t start,
                                             const can_flash_host_frame_t *header, app_control_info_t *info)
{
    uint8_t frames[APP_CONTROL_INFO_BYTES];
    memcpy(frames, header->data, CAN_FLASH_HOST_DLC);
    for (uint32_t off = CAN_FLASH_HOST_DLC; off < sizeof(frames); off += CAN_FLASH_HOST_DLC) {
        can_flash_host_frame_t f;
        const can_flash_host_status_t status = app_frame(h, reply_id, start, &f);
        if (status != CAN_FLASH_HOST_STATUS_OK) { return status; }
        memcpy(&frames[off], f.data, CAN_FLASH_HOST_DLC);
    }
    return app_control_decode_info(frames, info) ? CAN_FLASH_HOST_STATUS_OK : CAN_FLASH_HOST_STATUS_PROTOCOL;
}

/* One app-control request with bounded retries; info is NULL for ENTER. */
static can_flash_host_status_t app_request(can_flash_host_t *h, uint16_t cmd_id, uint16_t reply_id,
                                           uint8_t cmd, app_control_info_t *info)
{
    uint8_t request[CAN_FLASH_HOST_DLC];
    app_control_encode_request(cmd, request);
    h->result.step = (can_flash_host_step_t)cmd;
    bool answered = false;  /* something spoke on reply_id, never validly */
    for (uint32_t attempt = 1u; attempt <= CAN_FLASH_HOST_APP_ATTEMPTS; ++attempt) {
        h->result.attempts = attempt;
        if (flush_queued(h) != CAN_FLASH_HOST_STATUS_OK ||
            send_frame(h, cmd_id, request) != CAN_FLASH_HOST_STATUS_OK) { return h->result.status; }
        const uint32_t start = h->clock_ms(h->user);
        can_flash_host_frame_t f;
        app_control_reply_t kind = APP_CONTROL_REPLY_FOREIGN;
        can_flash_host_status_t status;
        do {
            status = app_frame(h, reply_id, start, &f);
            if (status == CAN_FLASH_HOST_STATUS_OK) {
                kind = app_control_classify_reply(cmd, f.data);
                /* Our echo with nonsense is malformed; other replies on the
                 * ID (stale HELLO/INFO answers) are just skipped. */
                if (kind == APP_CONTROL_REPLY_FOREIGN && f.data[0] == CAN_UPDATE_REPLY_MARKER &&
                    f.data[1] == cmd) { answered = true; }
            } else if (status == CAN_FLASH_HOST_STATUS_PROTOCOL) {
                answered = true;
            }
        } while (kind == APP_CONTROL_REPLY_FOREIGN &&
                 (status == CAN_FLASH_HOST_STATUS_OK || status == CAN_FLASH_HOST_STATUS_PROTOCOL));
        if (status == CAN_FLASH_HOST_STATUS_TRANSPORT) { return status; }
        if (status == CAN_FLASH_HOST_STATUS_TIMEOUT) { continue; }
        h->result.nak = f.data[2];
        h->result.arg = f.data[3];
        if (kind == APP_CONTROL_REPLY_UPDATER_NAK) { return outcome(h, CAN_FLASH_HOST_STATUS_IN_UPDATER); }
        if (kind == APP_CONTROL_REPLY_APP_NAK) { return outcome(h, CAN_FLASH_HOST_STATUS_NAK); }
        if (info == NULL) { return outcome(h, CAN_FLASH_HOST_STATUS_OK); }
        status = app_info_body(h, reply_id, start, &f, info);
        if (status == CAN_FLASH_HOST_STATUS_OK || status == CAN_FLASH_HOST_STATUS_TRANSPORT) {
            return outcome(h, status);
        }
        answered = true;  /* header without a complete, valid burst */
    }
    return outcome(h, answered ? CAN_FLASH_HOST_STATUS_PROTOCOL : CAN_FLASH_HOST_STATUS_TIMEOUT);
}

static bool valid_ids(const can_flash_host_t *h, uint16_t cmd_id, uint16_t reply_id)
{
    return valid_host(h) && cmd_id <= DFU_CONFIG_CAN_ID_MAX && reply_id <= DFU_CONFIG_CAN_ID_MAX;
}

can_flash_host_status_t can_flash_host_app_info(can_flash_host_t *h, uint16_t cmd_id,
                                               uint16_t reply_id, app_control_info_t *info)
{
    if (info == NULL || !valid_ids(h, cmd_id, reply_id)) { return CAN_FLASH_HOST_STATUS_INVALID_ARG; }
    memset(&h->result, 0, sizeof(h->result));
    memset(info, 0, sizeof(*info));
    return app_request(h, cmd_id, reply_id, CAN_UPDATE_CMD_APP_INFO, info);
}

can_flash_host_status_t can_flash_host_enter_updater(can_flash_host_t *h, uint16_t cmd_id,
                                                    uint16_t reply_id)
{
    if (!valid_ids(h, cmd_id, reply_id)) { return CAN_FLASH_HOST_STATUS_INVALID_ARG; }
    memset(&h->result, 0, sizeof(h->result));
    return app_request(h, cmd_id, reply_id, CAN_UPDATE_CMD_ENTER_UPDATER, NULL);
}

void can_flash_host_result_text(const can_flash_host_result_t *r, char *buf, size_t len)
{
    static const char *const steps[] = { "NONE", "HELLO", "ERASE", "DATA", "SIZE", "VERIFY", "GO", "INFO", "APP_INFO", "ENTER_UPDATER" };
    static const char *const errors[] = { "OK", "unexpected sequence", "flash failure", "CRC mismatch", "size out of bounds", "bad state", "image rejected" };
    static const char *const statuses[] = { "success", "invalid argument", "transport failure", "timeout", "rejected", "invalid reply", "image read failed", "scan capacity exceeded", "node is in its updater" };
    if (r == NULL || buf == NULL || len == 0u) { return; }
    const char *step = (unsigned)r->step < sizeof(steps) / sizeof(steps[0]) ? steps[r->step] : "?";
    const char *status = (unsigned)r->status < sizeof(statuses) / sizeof(statuses[0]) ? statuses[r->status] : "?";
    if (r->status == CAN_FLASH_HOST_STATUS_NAK) {
        (void)snprintf(buf, len, "%s rejected: %s 0x%02X (window %" PRIu32 ", arg 0x%02X)", step,
            r->nak < sizeof(errors) / sizeof(errors[0]) ? errors[r->nak] : "unknown NAK", r->nak, r->window, r->arg);
    } else if (r->step == CAN_FLASH_HOST_STEP_DATA && r->status == CAN_FLASH_HOST_STATUS_TIMEOUT) {
        (void)snprintf(buf, len, "DATA window %" PRIu32 " not accepted after %" PRIu32 " attempts", r->window, r->attempts);
    } else {
        (void)snprintf(buf, len, "%s: %s (attempts %" PRIu32 ")", step, status, r->attempts);
    }
}
