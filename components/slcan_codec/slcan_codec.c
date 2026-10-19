/**
 * @file    slcan_codec.c
 * @brief   SLCAN (Lawicel) line parser and encoder (see slcan_codec.h for the contract).
 *
 * Implementation notes: hex fields are parsed digit by digit against an exact length; the
 * bitrate table is Lawicel S0..S8, python-can's table without its non-standard S9.
 *
 * @see     slcan_codec.h, test_slcan_codec.c
 */

#include "slcan_codec.h"

#include <string.h>


/* -----------------------------------------------------------------------------
 * Constants
 * ----------------------------------------------------------------------------- */
#define STD_ID_DIGITS       3u
#define EXT_ID_DIGITS       8u
#define SERIAL_DIGITS       4u
#define VERSION_PART_MAX    99u
#define BITRATE_CMD_LEN     2u     /* "Sn" */
#define BARE_CMD_LEN        1u     /* "O", "C", "V", ... */

/* Lawicel S0..S8 (python-can's _BITRATES without its non-standard S9). */
static const uint32_t k_bitrates_bps[] = {
    10000u, 20000u, 50000u, 100000u, 125000u, 250000u, 500000u, 750000u, 1000000u,
};
#define BITRATE_COUNT  (sizeof(k_bitrates_bps) / sizeof(k_bitrates_bps[0]))

static const char k_hex_digits[] = "0123456789ABCDEF";


/* -----------------------------------------------------------------------------
 * Private helpers
 * ----------------------------------------------------------------------------- */
static bool parse_hex(const char *text, size_t digits, uint32_t *value)
{
    uint32_t result = 0u;
    for (size_t i = 0u; i < digits; i++) {
        const char c = text[i];
        uint32_t nibble;
        if (c >= '0' && c <= '9') {
            nibble = (uint32_t)(c - '0');
        } else if (c >= 'A' && c <= 'F') {
            nibble = (uint32_t)(c - 'A') + 10u;
        } else if (c >= 'a' && c <= 'f') {
            nibble = (uint32_t)(c - 'a') + 10u;
        } else {
            return false;
        }
        result = (result << 4) | nibble;
    }
    *value = result;
    return true;
}

static void put_hex(char *out, uint32_t value, size_t digits)
{
    for (size_t i = digits; i > 0u; i--) {
        out[i - 1u] = k_hex_digits[value & 0xFu];
        value >>= 4;
    }
}

static slcan_status_t parse_bare(size_t len, slcan_cmd_type_t type, slcan_command_t *out)
{
    if (len != BARE_CMD_LEN) {
        return SLCAN_STATUS_ERROR;
    }
    out->type = type;
    return SLCAN_STATUS_OK;
}

static slcan_status_t parse_bitrate(const char *line, size_t len, slcan_command_t *out)
{
    if (len != BITRATE_CMD_LEN || line[1] < '0' ||
        (size_t)(line[1] - '0') >= BITRATE_COUNT) {
        return SLCAN_STATUS_ERROR;
    }
    out->type        = SLCAN_CMD_SET_BITRATE;
    out->bitrate_bps = k_bitrates_bps[line[1] - '0'];
    return SLCAN_STATUS_OK;
}

/* t / T / r / R: kind letter, ID, one DLC digit, then 2 hex digits per data
 * byte for data frames only; any other length is malformed. */
static slcan_status_t parse_frame(const char *line, size_t len, slcan_command_t *out)
{
    const char kind      = line[0];
    const bool extended  = (kind == 'T' || kind == 'R');
    const bool remote    = (kind == 'r' || kind == 'R');
    const size_t id_len  = extended ? EXT_ID_DIGITS : STD_ID_DIGITS;
    const size_t dlc_pos = 1u + id_len;

    if (len <= dlc_pos) {
        return SLCAN_STATUS_ERROR;
    }
    uint32_t id;
    if (!parse_hex(&line[1], id_len, &id) ||
        id > (extended ? SLCAN_EXT_ID_MAX : SLCAN_STD_ID_MAX)) {
        return SLCAN_STATUS_ERROR;
    }
    const char dlc_char = line[dlc_pos];
    if (dlc_char < '0' || dlc_char > (char)('0' + SLCAN_MAX_DLC)) {
        return SLCAN_STATUS_ERROR;
    }
    const uint8_t dlc       = (uint8_t)(dlc_char - '0');
    const size_t data_chars = remote ? 0u : 2u * dlc;
    if (len != dlc_pos + 1u + data_chars) {
        return SLCAN_STATUS_ERROR;
    }

    slcan_frame_t *frame = &out->frame;
    for (size_t i = 0u; i < data_chars / 2u; i++) {
        uint32_t byte;
        if (!parse_hex(&line[dlc_pos + 1u + 2u * i], 2u, &byte)) {
            return SLCAN_STATUS_ERROR;
        }
        frame->data[i] = (uint8_t)byte;
    }
    frame->id       = id;
    frame->dlc      = dlc;
    frame->extended = extended;
    frame->remote   = remote;
    out->type       = SLCAN_CMD_TRANSMIT;
    return SLCAN_STATUS_OK;
}


/* -----------------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------------- */
slcan_status_t slcan_parse_line(const char *line, size_t len, slcan_command_t *out)
{
    if (out == NULL || (line == NULL && len != 0u)) {
        return SLCAN_STATUS_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    if (len == 0u) {
        out->type = SLCAN_CMD_EMPTY;
        return SLCAN_STATUS_OK;
    }

    slcan_status_t status;
    switch (line[0]) {
    case 'O': status = parse_bare(len, SLCAN_CMD_OPEN, out);    break;
    case 'L': status = parse_bare(len, SLCAN_CMD_LISTEN, out);  break;
    case 'C': status = parse_bare(len, SLCAN_CMD_CLOSE, out);   break;
    case 'V': status = parse_bare(len, SLCAN_CMD_VERSION, out); break;
    case 'N': status = parse_bare(len, SLCAN_CMD_SERIAL, out);  break;
    case 'S': status = parse_bitrate(line, len, out);           break;
    case 't':
    case 'T':
    case 'r':
    case 'R': status = parse_frame(line, len, out);             break;
    default:  status = SLCAN_STATUS_ERROR;                      break;
    }

    if (status != SLCAN_STATUS_OK) {
        memset(out, 0, sizeof(*out));  /* no half-parsed command escapes */
    }
    return status;
}


slcan_status_t slcan_encode_frame(const slcan_frame_t *frame, char *out, size_t out_size,
                                  size_t *out_len)
{
    if (frame == NULL || out == NULL || out_len == NULL ||
        frame->id > (frame->extended ? SLCAN_EXT_ID_MAX : SLCAN_STD_ID_MAX)) {
        return SLCAN_STATUS_INVALID_ARG;
    }

    /* Classic CAN: DLC 9..15 still carries 8 bytes, and SLCAN has one digit. */
    const uint8_t dlc       = (frame->dlc < SLCAN_MAX_DLC) ? frame->dlc : (uint8_t)SLCAN_MAX_DLC;
    const size_t id_len     = frame->extended ? EXT_ID_DIGITS : STD_ID_DIGITS;
    const size_t data_bytes = frame->remote ? 0u : dlc;
    const size_t needed     = 1u + id_len + 1u + 2u * data_bytes + 1u;
    if (out_size < needed) {
        return SLCAN_STATUS_INVALID_ARG;
    }

    size_t pos = 0u;
    if (frame->remote) {
        out[pos++] = frame->extended ? 'R' : 'r';
    } else {
        out[pos++] = frame->extended ? 'T' : 't';
    }
    put_hex(&out[pos], frame->id, id_len);
    pos += id_len;
    out[pos++] = (char)('0' + dlc);
    for (size_t i = 0u; i < data_bytes; i++) {
        put_hex(&out[pos], frame->data[i], 2u);
        pos += 2u;
    }
    out[pos++] = '\r';

    *out_len = pos;
    return SLCAN_STATUS_OK;
}


slcan_status_t slcan_encode_version(uint8_t hw_version, uint8_t sw_version, char *out,
                                    size_t out_size, size_t *out_len)
{
    if (out == NULL || out_len == NULL || out_size < SLCAN_REPLY_LINE_MAX ||
        hw_version > VERSION_PART_MAX || sw_version > VERSION_PART_MAX) {
        return SLCAN_STATUS_INVALID_ARG;
    }
    out[0] = 'V';
    out[1] = (char)('0' + hw_version / 10u);
    out[2] = (char)('0' + hw_version % 10u);
    out[3] = (char)('0' + sw_version / 10u);
    out[4] = (char)('0' + sw_version % 10u);
    out[5] = '\r';
    *out_len = SLCAN_REPLY_LINE_MAX;
    return SLCAN_STATUS_OK;
}


slcan_status_t slcan_encode_serial(uint16_t serial, char *out, size_t out_size,
                                   size_t *out_len)
{
    if (out == NULL || out_len == NULL || out_size < SLCAN_REPLY_LINE_MAX) {
        return SLCAN_STATUS_INVALID_ARG;
    }
    out[0] = 'N';
    put_hex(&out[1], serial, SERIAL_DIGITS);
    out[1u + SERIAL_DIGITS] = '\r';
    *out_len = SLCAN_REPLY_LINE_MAX;
    return SLCAN_STATUS_OK;
}
