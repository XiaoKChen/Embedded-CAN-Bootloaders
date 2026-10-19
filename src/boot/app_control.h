/**
 * @file    app_control.h
 * @brief   Codec for the CAN Update Protocol v1 app-control extension (APP_INFO, ENTER_UPDATER).
 *
 * APP_INFO / ENTER_UPDATER are answered by a RUNNING node app on the node's own update IDs. The
 * updater's engine NAKs both as unknown commands, which is what lets a host tell "app answered"
 * from "node already parked in its updater".
 *
 * Owns:         The app-control request/reply encoding, reply classification and the 9-frame
 *               APP_INFO layout, shared by both ends: the node app parses requests and encodes
 *               replies; the host (can_flash_host, tools/can-flash) encodes requests and
 *               classifies/decodes replies.
 * Not here:     Opcodes, markers and the frame layout (can_update.h); moving frames on the bus
 *               (examples/node_app can_service.c, can_flash_host).
 * Errors:       A bad DLC/version/key parses as APP_CONTROL_REQ_MALFORMED;
 *               app_control_decode_info() fails closed (false, out untouched).
 * Depends on:   can_update.h only. Pure byte-buffer codec: no hardware, no heap.
 *
 * @see     app_control.c, can_update.h, test_app_control.c
 */

#ifndef APP_CONTROL_H
#define APP_CONTROL_H

#include <stdbool.h>
#include <stdint.h>

#include "can_update.h"

#ifdef __cplusplus
extern "C" {
#endif


/* -----------------------------------------------------------------------------
 * Constants
 * ----------------------------------------------------------------------------- */
#define APP_CONTROL_FRAME_BYTES   8u
#define APP_CONTROL_STR_MAX       32u   /* esp_app_desc_t version/project_name */
#define APP_CONTROL_FIELD_FRAMES  (APP_CONTROL_STR_MAX / APP_CONTROL_FRAME_BYTES)
#define APP_CONTROL_INFO_FRAMES   (1u + 2u * APP_CONTROL_FIELD_FRAMES)  /* 9 */
#define APP_CONTROL_INFO_BYTES    (APP_CONTROL_INFO_FRAMES * APP_CONTROL_FRAME_BYTES)


/* -----------------------------------------------------------------------------
 * Types
 * ----------------------------------------------------------------------------- */
typedef enum {
    APP_CONTROL_REQ_NONE = 0,       /* not an app-control opcode: an app ignores
                                       it (HELLO, INFO, DATA, ... are updater-only) */
    APP_CONTROL_REQ_APP_INFO,
    APP_CONTROL_REQ_ENTER_UPDATER,
    APP_CONTROL_REQ_MALFORMED,      /* app-control opcode, bad DLC/version/key:
                                       NAK it, never act on it                */
} app_control_request_t;

typedef enum {
    APP_CONTROL_REPLY_FOREIGN = 0,  /* not a valid reply to this command       */
    APP_CONTROL_REPLY_APP_ACK,      /* a running app accepted                  */
    APP_CONTROL_REPLY_APP_NAK,      /* a running app refused (err in byte2)    */
    APP_CONTROL_REPLY_UPDATER_NAK,  /* the updater's unknown-command NAK       */
} app_control_reply_t;

typedef struct {
    uint8_t protocol_version;
    uint8_t version_len;
    uint8_t project_len;
    char    version[APP_CONTROL_STR_MAX + 1u];       /* NUL-terminated */
    char    project_name[APP_CONTROL_STR_MAX + 1u];  /* NUL-terminated */
} app_control_info_t;


/* -----------------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------------- */

/** Host: the 8-byte request for CAN_UPDATE_CMD_APP_INFO or _ENTER_UPDATER. */
void app_control_encode_request(uint8_t cmd, uint8_t out[APP_CONTROL_FRAME_BYTES]);

/** App: classify one frame received on the app's own cmd ID. */
app_control_request_t app_control_parse_request(uint8_t dlc,
                                                const uint8_t data[APP_CONTROL_FRAME_BYTES]);

/** App: the single-frame ACK (err = CAN_UPDATE_OK) or NAK for `cmd`. */
void app_control_encode_reply(uint8_t cmd, uint8_t err, uint8_t out[APP_CONTROL_FRAME_BYTES]);

/** Host: classify a DLC-8 frame from the reply ID against the command sent. */
app_control_reply_t app_control_classify_reply(uint8_t cmd,
                                               const uint8_t data[APP_CONTROL_FRAME_BYTES]);

/**
 * App: the 9-frame APP_INFO reply, frame i at frames[i * 8]. Strings longer
 * than APP_CONTROL_STR_MAX are truncated; NULL encodes as empty.
 */
void app_control_encode_info(const char *version, const char *project_name,
                             uint8_t frames[APP_CONTROL_INFO_BYTES]);

/**
 * Host: decode a complete 9-frame APP_INFO reply (frame i at frames[i * 8],
 * in arrival order). Fails closed (false, out
 * untouched) on a wrong header, a length > 32, a NUL inside a string, a
 * non-zero pad byte or a check-byte mismatch — i.e. a partial or foreign burst.
 */
bool app_control_decode_info(const uint8_t frames[APP_CONTROL_INFO_BYTES],
                             app_control_info_t *out);


#ifdef __cplusplus
}
#endif

#endif /* APP_CONTROL_H */
