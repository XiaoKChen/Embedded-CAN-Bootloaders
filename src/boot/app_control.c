/* =============================================================================
 * app_control.c — CAN Update Protocol v1 app-control extension codec
 * =============================================================================
 * See app_control.h and the protocol block in can_update.h.
 * ============================================================================= */

#include "app_control.h"

#include <stddef.h>
#include <string.h>

#include "crc32.h"


/* -----------------------------------------------------------------------------
 * Constants: APP_INFO header layout
 * ----------------------------------------------------------------------------- */
#define HDR_MARKER       0u
#define HDR_CMD          1u
#define HDR_ERR          2u
#define HDR_ARG          3u
#define HDR_PROTOCOL     4u
#define HDR_VERSION_LEN  5u
#define HDR_PROJECT_LEN  6u
#define HDR_CHECK        7u
#define FIELDS_OFFSET    APP_CONTROL_FRAME_BYTES                 /* frame 1    */
#define VERSION_OFFSET   FIELDS_OFFSET                           /* frames 1-4 */
#define PROJECT_OFFSET   (VERSION_OFFSET + APP_CONTROL_STR_MAX)  /* frames 5-8 */


/* -----------------------------------------------------------------------------
 * Private helpers
 * ----------------------------------------------------------------------------- */
static uint32_t load_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void store_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint8_t bounded_len(const char *text)
{
    uint8_t len = 0u;
    while (text != NULL && len < APP_CONTROL_STR_MAX && text[len] != '\0') {
        len++;
    }
    return len;
}

/* Low byte of CRC32 over header bytes 4..6 and both 32-byte fields: the
 * frames carry no sequence numbers, so this is what rejects a burst with a
 * lost, repeated or foreign frame on the reply ID. */
static uint8_t info_check(const uint8_t frames[APP_CONTROL_INFO_BYTES])
{
    const uint32_t crc = crc32_ieee_update(crc32_ieee(&frames[HDR_PROTOCOL], 3u),
                                           &frames[FIELDS_OFFSET], 2u * APP_CONTROL_STR_MAX);
    return (uint8_t)(crc & 0xFFu);
}

static void encode_field(const char *text, uint8_t len, uint8_t field[APP_CONTROL_STR_MAX])
{
    memset(field, 0x00, APP_CONTROL_STR_MAX);
    if (len != 0u) {
        memcpy(field, text, len);
    }
}

/* Valid when every byte inside `len` is non-NUL and every pad byte past it
 * is 0x00. */
static bool decode_field(const uint8_t field[APP_CONTROL_STR_MAX], uint8_t len,
                         char out[APP_CONTROL_STR_MAX + 1u])
{
    for (uint32_t i = 0u; i < APP_CONTROL_STR_MAX; i++) {
        const bool inside = (i < len);
        if (inside == (field[i] == 0x00u)) {
            return false;
        }
    }
    memcpy(out, field, len);
    out[len] = '\0';
    return true;
}


/* -----------------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------------- */
void app_control_encode_request(uint8_t cmd, uint8_t out[APP_CONTROL_FRAME_BYTES])
{
    memset(out, CAN_UPDATE_FILL_BYTE, APP_CONTROL_FRAME_BYTES);
    out[0] = cmd;
    out[1] = CAN_UPDATE_PROTOCOL_VERSION;
    if (cmd == CAN_UPDATE_CMD_ENTER_UPDATER) {
        store_le32(&out[2], CAN_UPDATE_ENTER_KEY);
    }
}

app_control_request_t app_control_parse_request(uint8_t dlc,
                                                const uint8_t data[APP_CONTROL_FRAME_BYTES])
{
    const uint8_t cmd = data[0];
    if (cmd != CAN_UPDATE_CMD_APP_INFO && cmd != CAN_UPDATE_CMD_ENTER_UPDATER) {
        return APP_CONTROL_REQ_NONE;
    }
    if (dlc != APP_CONTROL_FRAME_BYTES) {
        return APP_CONTROL_REQ_MALFORMED;
    }
    if (cmd == CAN_UPDATE_CMD_APP_INFO) {
        return APP_CONTROL_REQ_APP_INFO;  /* any version: the reply states ours */
    }
    /* Parking a running node is consequential: fail closed on any doubt. */
    if (data[1] != CAN_UPDATE_PROTOCOL_VERSION || load_le32(&data[2]) != CAN_UPDATE_ENTER_KEY) {
        return APP_CONTROL_REQ_MALFORMED;
    }
    return APP_CONTROL_REQ_ENTER_UPDATER;
}

void app_control_encode_reply(uint8_t cmd, uint8_t err, uint8_t out[APP_CONTROL_FRAME_BYTES])
{
    out[HDR_MARKER] = CAN_UPDATE_REPLY_MARKER;
    out[HDR_CMD]    = cmd;
    out[HDR_ERR]    = err;
    out[HDR_ARG]    = CAN_UPDATE_APP_MARKER;
    memset(&out[4], CAN_UPDATE_FILL_BYTE, 4u);
}

app_control_reply_t app_control_classify_reply(uint8_t cmd,
                                               const uint8_t data[APP_CONTROL_FRAME_BYTES])
{
    if (data[HDR_MARKER] != CAN_UPDATE_REPLY_MARKER || data[HDR_CMD] != cmd) {
        return APP_CONTROL_REPLY_FOREIGN;
    }
    if (data[HDR_ARG] == CAN_UPDATE_APP_MARKER) {
        return (data[HDR_ERR] == CAN_UPDATE_OK) ? APP_CONTROL_REPLY_APP_ACK
                                                : APP_CONTROL_REPLY_APP_NAK;
    }
    if (data[HDR_ERR] == CAN_UPDATE_ERR_STATE && data[HDR_ARG] == 0x00u) {
        return APP_CONTROL_REPLY_UPDATER_NAK;
    }
    return APP_CONTROL_REPLY_FOREIGN;
}

void app_control_encode_info(const char *version, const char *project_name,
                             uint8_t frames[APP_CONTROL_INFO_BYTES])
{
    const uint8_t version_len = bounded_len(version);
    const uint8_t project_len = bounded_len(project_name);

    app_control_encode_reply(CAN_UPDATE_CMD_APP_INFO, CAN_UPDATE_OK, frames);
    frames[HDR_PROTOCOL]    = CAN_UPDATE_PROTOCOL_VERSION;
    frames[HDR_VERSION_LEN] = version_len;
    frames[HDR_PROJECT_LEN] = project_len;
    encode_field(version, version_len, &frames[VERSION_OFFSET]);
    encode_field(project_name, project_len, &frames[PROJECT_OFFSET]);
    frames[HDR_CHECK] = info_check(frames);
}

bool app_control_decode_info(const uint8_t frames[APP_CONTROL_INFO_BYTES],
                             app_control_info_t *out)
{
    if (app_control_classify_reply(CAN_UPDATE_CMD_APP_INFO, frames) != APP_CONTROL_REPLY_APP_ACK ||
        frames[HDR_VERSION_LEN] > APP_CONTROL_STR_MAX ||
        frames[HDR_PROJECT_LEN] > APP_CONTROL_STR_MAX ||
        frames[HDR_CHECK] != info_check(frames)) {
        return false;
    }

    app_control_info_t info;
    info.protocol_version = frames[HDR_PROTOCOL];
    info.version_len      = frames[HDR_VERSION_LEN];
    info.project_len      = frames[HDR_PROJECT_LEN];
    if (!decode_field(&frames[VERSION_OFFSET], info.version_len, info.version) ||
        !decode_field(&frames[PROJECT_OFFSET], info.project_len, info.project_name)) {
        return false;
    }
    *out = info;
    return true;
}
