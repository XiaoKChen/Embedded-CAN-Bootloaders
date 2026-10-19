/**
 * @file    can_update.c
 * @brief   CAN Update Protocol v1 state machine (see can_update.h for the contract).
 *
 * Implementation notes: every slot access goes through the injected HAL. The slot is handed to
 * the target for acceptance only after its read-back CRC matches VERIFY, and made bootable only
 * on GO, so an interrupted update always leaves the device coming back up in the updater.
 *
 * @see     can_update.h, test_can_update.c
 */

#include "can_update.h"

#include <string.h>

#include "crc32.h"

/* -----------------------------------------------------------------------------
 * Private helpers
 * ----------------------------------------------------------------------------- */
static uint32_t load_le32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void store_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

/* Build an ACK (err == CAN_UPDATE_OK) or NAK reply. Returns true so call
 * sites can `return make_reply(...)` directly from handle_frame. */
static bool make_reply(uint8_t reply[8], uint8_t cmd_echo, uint8_t err, uint8_t arg)
{
    reply[0] = CAN_UPDATE_REPLY_MARKER;
    reply[1] = cmd_echo;
    reply[2] = err;
    reply[3] = arg;
    memset(&reply[4], CAN_UPDATE_FILL_BYTE, 4u);
    return true;
}

static uint32_t min_u32(uint32_t a, uint32_t b)
{
    return (a < b) ? a : b;
}

/* Rewind to the start of the current window: drop buffered bytes and expect
 * the window's first seq again. Completed blocks stay counted. */
static void discard_window(can_update_state_t *st)
{
    st->block_fill     = 0u;
    st->bytes_received = st->block_index * CAN_UPDATE_WINDOW_SIZE;
    st->expected_seq   = st->window_start_seq;
}

static void reset_session(can_update_state_t *st)
{
    st->phase            = CAN_UPDATE_PHASE_IDLE;
    st->image_size       = 0u;
    st->bytes_received   = 0u;
    st->block_index      = 0u;
    st->block_fill       = 0u;
    st->expected_seq     = 0u;
    st->window_start_seq = 0u;
}

/* -----------------------------------------------------------------------------
 * Command handlers — each returns true when a reply was produced
 * ----------------------------------------------------------------------------- */
static bool handle_hello(can_update_state_t *st, const uint8_t data[8], uint8_t reply[8])
{
    if (data[1] != CAN_UPDATE_PROTOCOL_VERSION) {
        return make_reply(reply, CAN_UPDATE_CMD_HELLO, CAN_UPDATE_ERR_STATE, 0u);
    }

    /* HELLO (re)starts the session from any phase — host-retry robustness. */
    reset_session(st);
    st->phase = CAN_UPDATE_PHASE_GREETED;

    (void)make_reply(reply, CAN_UPDATE_CMD_HELLO, CAN_UPDATE_OK,
                     CAN_UPDATE_PROTOCOL_VERSION);
    store_le32(&reply[4], st->hal->slot_size);  /* HELLO ACK carries the slot size */
    return true;
}

static bool handle_size(can_update_state_t *st, const uint8_t data[8], uint8_t reply[8])
{
    const uint32_t image_size = load_le32(&data[2]);

    if (image_size == 0u || image_size > st->hal->slot_size) {
        return make_reply(reply, CAN_UPDATE_CMD_SIZE, CAN_UPDATE_ERR_SIZE, 0u);
    }

    st->image_size = image_size;
    st->phase      = CAN_UPDATE_PHASE_SIZED;
    return make_reply(reply, CAN_UPDATE_CMD_SIZE, CAN_UPDATE_OK, 0u);
}

static bool handle_erase(can_update_state_t *st, uint8_t reply[8])
{
    /* Power loss during/after ERASE: the slot is not selected for boot
     * (HAL contract), so the device comes back up in the updater. */
    if (!st->hal->slot_erase()) {
        return make_reply(reply, CAN_UPDATE_CMD_ERASE, CAN_UPDATE_ERR_FLASH, 0u);
    }

    st->bytes_received   = 0u;
    st->block_index      = 0u;
    st->block_fill       = 0u;
    st->expected_seq     = 0u;
    st->window_start_seq = 0u;
    st->phase            = CAN_UPDATE_PHASE_TRANSFER;
    return make_reply(reply, CAN_UPDATE_CMD_ERASE, CAN_UPDATE_OK, 0u);
}

static bool handle_data(can_update_state_t *st, const uint8_t data[8], uint8_t reply[8])
{
    const uint8_t seq = data[1];

    if (seq != st->expected_seq) {
        const uint8_t rewind_seq = st->window_start_seq;
        discard_window(st);
        return make_reply(reply, CAN_UPDATE_CMD_DATA, CAN_UPDATE_ERR_SEQ, rewind_seq);
    }

    /* Frames never straddle windows (host packs 341x6 + 1x2 per window), but
     * clamp against both the window boundary and image_size regardless. */
    const uint32_t take = min_u32(CAN_UPDATE_DATA_BYTES_PER_FRAME,
                                  min_u32(CAN_UPDATE_WINDOW_SIZE - st->block_fill,
                                          st->image_size - st->bytes_received));
    memcpy(&st->block_buf[st->block_fill], &data[2], take);
    st->block_fill     += take;
    st->bytes_received += take;
    st->expected_seq++;  /* uint8_t: wraps mod 256 by definition */

    const bool block_full     = (st->block_fill == CAN_UPDATE_WINDOW_SIZE);
    const bool image_complete = (st->bytes_received == st->image_size);
    if (!block_full && !image_complete) {
        return false;  /* mid-window DATA frames get no reply */
    }

    /* Power loss during this program: slot torn but not selected for boot
     * -> device comes back up in the updater. */
    if (!st->hal->slot_program(st->block_index * CAN_UPDATE_WINDOW_SIZE,
                               st->block_buf, st->block_fill)) {
        discard_window(st);
        return make_reply(reply, CAN_UPDATE_CMD_DATA, CAN_UPDATE_ERR_FLASH, 0u);
    }

    st->block_index++;
    st->block_fill       = 0u;
    st->window_start_seq = st->expected_seq;
    if (image_complete) {
        st->phase = CAN_UPDATE_PHASE_TRANSFERRED;
    }
    return make_reply(reply, CAN_UPDATE_CMD_DATA, CAN_UPDATE_OK, seq);
}

/* CRC32 of the first image_size slot bytes, read back through the HAL in
 * window-sized chunks (block_buf is free once every window is programmed). */
static bool slot_crc32(can_update_state_t *st, uint32_t *out_crc)
{
    uint32_t crc = 0u;
    for (uint32_t offset = 0u; offset < st->image_size; offset += CAN_UPDATE_WINDOW_SIZE) {
        const uint32_t chunk = min_u32(CAN_UPDATE_WINDOW_SIZE, st->image_size - offset);
        if (!st->hal->slot_read(offset, st->block_buf, chunk)) {
            return false;
        }
        crc = crc32_ieee_update(crc, st->block_buf, chunk);
    }
    *out_crc = crc;
    return true;
}

static bool handle_verify(can_update_state_t *st, const uint8_t data[8], uint8_t reply[8])
{
    const uint32_t expected_crc = load_le32(&data[2]);
    uint32_t computed_crc;

    if (!slot_crc32(st, &computed_crc)) {
        return make_reply(reply, CAN_UPDATE_CMD_VERIFY, CAN_UPDATE_ERR_FLASH, 0u);
    }
    if (computed_crc != expected_crc) {
        /* Fail closed: the slot stays unaccepted and unbootable. */
        return make_reply(reply, CAN_UPDATE_CMD_VERIFY, CAN_UPDATE_ERR_CRC, 0u);
    }

    /* Bytes match what the host sent; now the target's own image checks.
     * Power loss here or after: the slot is still not selected for boot. */
    if (!st->hal->slot_commit()) {
        return make_reply(reply, CAN_UPDATE_CMD_VERIFY, CAN_UPDATE_ERR_IMAGE, 0u);
    }

    st->phase = CAN_UPDATE_PHASE_VERIFIED;
    return make_reply(reply, CAN_UPDATE_CMD_VERIFY, CAN_UPDATE_OK, 0u);
}

static bool handle_go(can_update_state_t *st, uint8_t reply[8],
                      can_update_action_t *out_action)
{
    /* The commit point: the target's atomic boot-selection write. On failure
     * the phase stays VERIFIED so the host may retry GO. */
    if (!st->hal->slot_activate()) {
        return make_reply(reply, CAN_UPDATE_CMD_GO, CAN_UPDATE_ERR_FLASH, 0u);
    }

    st->phase   = CAN_UPDATE_PHASE_IDLE;
    *out_action = CAN_UPDATE_ACTION_RESET;
    return make_reply(reply, CAN_UPDATE_CMD_GO, CAN_UPDATE_OK, 0u);
}

/* -----------------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------------- */
void can_update_init(can_update_state_t *st, const can_update_hal_t *hal,
                     uint32_t cmd_id)
{
    memset(st, 0, sizeof(*st));
    st->hal    = hal;
    st->cmd_id = cmd_id;  /* per-device host -> boot ID (see dfu_config.h v2) */
    reset_session(st);
}

bool can_update_handle_frame(can_update_state_t *st, uint32_t id, uint8_t dlc,
                             const uint8_t data[8], uint8_t reply_data[8],
                             can_update_action_t *out_action)
{
    *out_action = CAN_UPDATE_ACTION_NONE;

    if (id != st->cmd_id) {
        return false;  /* not ours — silently ignore */
    }

    const uint8_t cmd = data[0];

    if (dlc != 8u) {
        /* Protocol frames are always DLC 8; anything else is malformed. */
        return make_reply(reply_data, cmd, CAN_UPDATE_ERR_STATE, 0u);
    }

    /* HELLO restarts the session from any phase (see can_update.h). */
    if (cmd == CAN_UPDATE_CMD_HELLO) {
        return handle_hello(st, data, reply_data);
    }

    /* Every other command is valid in exactly one phase — fail closed. */
    switch (cmd) {
    case CAN_UPDATE_CMD_SIZE:
        if (st->phase == CAN_UPDATE_PHASE_GREETED) {
            return handle_size(st, data, reply_data);
        }
        break;
    case CAN_UPDATE_CMD_ERASE:
        if (st->phase == CAN_UPDATE_PHASE_SIZED) {
            return handle_erase(st, reply_data);
        }
        break;
    case CAN_UPDATE_CMD_DATA:
        if (st->phase == CAN_UPDATE_PHASE_TRANSFER) {
            return handle_data(st, data, reply_data);
        }
        break;
    case CAN_UPDATE_CMD_VERIFY:
        if (st->phase == CAN_UPDATE_PHASE_TRANSFERRED) {
            return handle_verify(st, data, reply_data);
        }
        break;
    case CAN_UPDATE_CMD_GO:
        if (st->phase == CAN_UPDATE_PHASE_VERIFIED) {
            return handle_go(st, reply_data, out_action);
        }
        break;
    default:
        break;  /* unknown command */
    }

    return make_reply(reply_data, cmd, CAN_UPDATE_ERR_STATE, 0u);
}
