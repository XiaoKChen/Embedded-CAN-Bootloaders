/**
 * @file    board_profile.c
 * @brief   Pure-logic parse/validate/encode of the ESP32 board-profile record (see
 *          board_profile.h for the contract).
 *
 * Implementation notes: fail-closed — parse writes the defaults first and overwrites them only
 * after the header, the crc32 and every field rule pass. All multi-byte fields go through
 * explicit little-endian byte loads/stores (mirrors dfu_config.c).
 *
 * @see     board_profile.h, test_board_profile.c
 */

#include "board_profile.h"

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

static uint64_t load_le64(const uint8_t *p)
{
    return (uint64_t)load_le32(p) | ((uint64_t)load_le32(p + 4) << 32);
}

static void store_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static int twai_pin_ok(uint8_t gpio)
{
    return gpio <= BOARD_PROFILE_MAX_GPIO &&
           (BOARD_PROFILE_REFUSED_MASK & (1ull << gpio)) == 0u;
}

/* -----------------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------------- */
void board_profile_defaults(board_profile_t *out)
{
    out->twai_tx_gpio  = BOARD_PROFILE_DEFAULT_TX_GPIO;
    out->twai_rx_gpio  = BOARD_PROFILE_DEFAULT_RX_GPIO;
    out->bitrate       = BOARD_PROFILE_DEFAULT_BITRATE;
    out->hold_low_mask = BOARD_PROFILE_DEFAULT_HOLD;
}

board_profile_status_t board_profile_check(const board_profile_t *p)
{
    const uint64_t valid_gpios = (1ull << (BOARD_PROFILE_MAX_GPIO + 1u)) - 1u;

    if (!twai_pin_ok(p->twai_tx_gpio) || !twai_pin_ok(p->twai_rx_gpio) ||
        p->twai_tx_gpio == p->twai_rx_gpio) {
        return BOARD_PROFILE_ERR_RULE;
    }
    if ((uint32_t)p->bitrate > (uint32_t)BOARD_PROFILE_BITRATE_1M) {
        return BOARD_PROFILE_ERR_RULE;
    }
    if ((p->hold_low_mask & ~valid_gpios) != 0u ||
        (p->hold_low_mask & BOARD_PROFILE_REFUSED_MASK) != 0u ||
        (p->hold_low_mask & ((1ull << p->twai_tx_gpio) | (1ull << p->twai_rx_gpio))) != 0u) {
        return BOARD_PROFILE_ERR_RULE;
    }
    return BOARD_PROFILE_OK;
}

board_profile_status_t board_profile_parse(const uint8_t *blob, size_t len,
                                           board_profile_t *out)
{
    board_profile_t decoded;

    board_profile_defaults(out);

    if (len < BOARD_PROFILE_SIZE) {
        return BOARD_PROFILE_ERR_SHORT;
    }
    if (load_le32(blob + BOARD_PROFILE_OFF_MAGIC) != BOARD_PROFILE_MAGIC) {
        return BOARD_PROFILE_ERR_MAGIC;
    }
    if (blob[BOARD_PROFILE_OFF_VERSION] != BOARD_PROFILE_VERSION) {
        return BOARD_PROFILE_ERR_VERSION;
    }
    if (blob[BOARD_PROFILE_OFF_LENGTH] != BOARD_PROFILE_SIZE) {
        return BOARD_PROFILE_ERR_LENGTH;
    }
    if (crc32_ieee(blob, BOARD_PROFILE_CRC_COVERAGE) !=
        load_le32(blob + BOARD_PROFILE_OFF_CRC)) {
        return BOARD_PROFILE_ERR_CRC;
    }

    decoded.twai_tx_gpio  = blob[BOARD_PROFILE_OFF_TX];
    decoded.twai_rx_gpio  = blob[BOARD_PROFILE_OFF_RX];
    decoded.bitrate       = (board_profile_bitrate_t)blob[BOARD_PROFILE_OFF_BITRATE];
    decoded.hold_low_mask = load_le64(blob + BOARD_PROFILE_OFF_HOLD_MASK);
    if (board_profile_check(&decoded) != BOARD_PROFILE_OK) {
        return BOARD_PROFILE_ERR_RULE;
    }

    *out = decoded;
    return BOARD_PROFILE_OK;
}

board_profile_status_t board_profile_encode(const board_profile_t *p,
                                            uint8_t out[BOARD_PROFILE_SIZE])
{
    uint8_t rec[BOARD_PROFILE_SIZE];

    if (board_profile_check(p) != BOARD_PROFILE_OK) {
        return BOARD_PROFILE_ERR_RULE;
    }

    store_le32(rec + BOARD_PROFILE_OFF_MAGIC, BOARD_PROFILE_MAGIC);
    rec[BOARD_PROFILE_OFF_VERSION]        = BOARD_PROFILE_VERSION;
    rec[BOARD_PROFILE_OFF_LENGTH]         = BOARD_PROFILE_SIZE;
    rec[BOARD_PROFILE_OFF_RESERVED16]     = 0xFFu;
    rec[BOARD_PROFILE_OFF_RESERVED16 + 1] = 0xFFu;
    rec[BOARD_PROFILE_OFF_TX]             = p->twai_tx_gpio;
    rec[BOARD_PROFILE_OFF_RX]             = p->twai_rx_gpio;
    rec[BOARD_PROFILE_OFF_BITRATE]        = (uint8_t)p->bitrate;
    rec[BOARD_PROFILE_OFF_RESERVED8]      = 0xFFu;
    store_le32(rec + BOARD_PROFILE_OFF_HOLD_MASK, (uint32_t)p->hold_low_mask);
    store_le32(rec + BOARD_PROFILE_OFF_HOLD_MASK + 4, (uint32_t)(p->hold_low_mask >> 32));
    store_le32(rec + BOARD_PROFILE_OFF_CRC, crc32_ieee(rec, BOARD_PROFILE_CRC_COVERAGE));

    memcpy(out, rec, sizeof rec);
    return BOARD_PROFILE_OK;
}

uint32_t board_profile_bitrate_bps(board_profile_bitrate_t code)
{
    switch (code) {
    case BOARD_PROFILE_BITRATE_125K: return 125000u;
    case BOARD_PROFILE_BITRATE_500K: return 500000u;
    case BOARD_PROFILE_BITRATE_1M:   return 1000000u;
    case BOARD_PROFILE_BITRATE_250K:
    default:                         return 250000u;
    }
}
