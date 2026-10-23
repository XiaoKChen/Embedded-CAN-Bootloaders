/**
 * @file    board_profile.c
 * @brief   Pure-logic parse/validate/encode of the ESP32 board-profile record (see
 *          board_profile.h for the contract).
 *
 * Implementation notes: fail-closed — parse writes the defaults first and overwrites them only
 * after the header, the crc32 and every field rule pass. The version byte selects the layout
 * (length, CRC coverage and offset); a v1 record decodes onto the defaults for the v2-only
 * fields. Both encoders build the record in a local buffer and copy it out only on success.
 * All multi-byte fields go through explicit little-endian byte loads/stores (mirrors
 * dfu_config.c).
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

static void store_le64(uint8_t *p, uint64_t v)
{
    store_le32(p, (uint32_t)v);
    store_le32(p + 4, (uint32_t)(v >> 32));
}

/* A GPIO the record may assign (TWAI or I2C): on the chip and not refused. */
static int pin_ok(uint8_t gpio)
{
    return gpio <= BOARD_PROFILE_MAX_GPIO &&
           (BOARD_PROFILE_REFUSED_MASK & (1ull << gpio)) == 0u;
}

static int hold_mask_ok(uint64_t mask, uint64_t twai_pins)
{
    const uint64_t valid_gpios = (1ull << (BOARD_PROFILE_MAX_GPIO + 1u)) - 1u;

    return (mask & ~valid_gpios) == 0u &&
           (mask & BOARD_PROFILE_REFUSED_MASK) == 0u &&
           (mask & twai_pins) == 0u;
}

static int i2c_ok(const board_profile_t *p, uint64_t twai_pins)
{
    if (p->i2c_sda_gpio == BOARD_PROFILE_I2C_NONE) {
        return p->i2c_scl_gpio == BOARD_PROFILE_I2C_NONE &&
               p->i2c_addr7 == BOARD_PROFILE_I2C_NONE &&
               p->i2c_write_count == 0u;
    }
    if (!pin_ok(p->i2c_sda_gpio) || !pin_ok(p->i2c_scl_gpio) ||
        p->i2c_sda_gpio == p->i2c_scl_gpio) {
        return 0;
    }
    const uint64_t i2c_pins = (1ull << p->i2c_sda_gpio) | (1ull << p->i2c_scl_gpio);
    if ((i2c_pins & (twai_pins | p->hold_low_mask | p->hold_high_mask)) != 0u) {
        return 0;
    }
    return p->i2c_addr7 >= BOARD_PROFILE_I2C_ADDR_MIN &&
           p->i2c_addr7 <= BOARD_PROFILE_I2C_ADDR_MAX &&
           p->i2c_write_count >= 1u &&
           p->i2c_write_count <= BOARD_PROFILE_I2C_MAX_WRITES;
}

/* Bytes 0x00..0x13 except the version/length pair, common to both layouts. */
static void encode_common(const board_profile_t *p, uint8_t *rec)
{
    store_le32(rec + BOARD_PROFILE_OFF_MAGIC, BOARD_PROFILE_MAGIC);
    rec[BOARD_PROFILE_OFF_RESERVED16]     = 0xFFu;
    rec[BOARD_PROFILE_OFF_RESERVED16 + 1] = 0xFFu;
    rec[BOARD_PROFILE_OFF_TX]             = p->twai_tx_gpio;
    rec[BOARD_PROFILE_OFF_RX]             = p->twai_rx_gpio;
    rec[BOARD_PROFILE_OFF_BITRATE]        = (uint8_t)p->bitrate;
    rec[BOARD_PROFILE_OFF_RESERVED8]      = 0xFFu;
    store_le64(rec + BOARD_PROFILE_OFF_HOLD_MASK, p->hold_low_mask);
}

/* -----------------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------------- */
void board_profile_defaults(board_profile_t *out)
{
    out->twai_tx_gpio    = BOARD_PROFILE_DEFAULT_TX_GPIO;
    out->twai_rx_gpio    = BOARD_PROFILE_DEFAULT_RX_GPIO;
    out->bitrate         = BOARD_PROFILE_DEFAULT_BITRATE;
    out->hold_low_mask   = BOARD_PROFILE_DEFAULT_HOLD;
    out->hold_high_mask  = BOARD_PROFILE_DEFAULT_HOLD;
    out->i2c_sda_gpio    = BOARD_PROFILE_I2C_NONE;
    out->i2c_scl_gpio    = BOARD_PROFILE_I2C_NONE;
    out->i2c_addr7       = BOARD_PROFILE_I2C_NONE;
    out->i2c_write_count = 0u;
    memset(out->i2c_writes, 0xFF, sizeof out->i2c_writes);
}

board_profile_status_t board_profile_check(const board_profile_t *p)
{
    if (!pin_ok(p->twai_tx_gpio) || !pin_ok(p->twai_rx_gpio) ||
        p->twai_tx_gpio == p->twai_rx_gpio) {
        return BOARD_PROFILE_ERR_RULE;
    }
    if ((uint32_t)p->bitrate > (uint32_t)BOARD_PROFILE_BITRATE_1M) {
        return BOARD_PROFILE_ERR_RULE;
    }
    const uint64_t twai_pins = (1ull << p->twai_tx_gpio) | (1ull << p->twai_rx_gpio);
    if (!hold_mask_ok(p->hold_low_mask, twai_pins) ||
        !hold_mask_ok(p->hold_high_mask, twai_pins) ||
        (p->hold_low_mask & p->hold_high_mask) != 0u) {
        return BOARD_PROFILE_ERR_RULE;
    }
    if (!i2c_ok(p, twai_pins)) {
        return BOARD_PROFILE_ERR_RULE;
    }
    return BOARD_PROFILE_OK;
}

board_profile_status_t board_profile_parse(const uint8_t *blob, size_t len,
                                           board_profile_t *out)
{
    board_profile_t decoded;
    uint32_t        size;
    uint32_t        crc_coverage;   /* the crc32 field follows its coverage directly */

    board_profile_defaults(out);

    if (len < BOARD_PROFILE_SIZE_V1) {
        return BOARD_PROFILE_ERR_SHORT;
    }
    if (load_le32(blob + BOARD_PROFILE_OFF_MAGIC) != BOARD_PROFILE_MAGIC) {
        return BOARD_PROFILE_ERR_MAGIC;
    }
    if (blob[BOARD_PROFILE_OFF_VERSION] == BOARD_PROFILE_VERSION_V1) {
        size         = BOARD_PROFILE_SIZE_V1;
        crc_coverage = BOARD_PROFILE_CRC_COVERAGE_V1;
    } else if (blob[BOARD_PROFILE_OFF_VERSION] == BOARD_PROFILE_VERSION_V2) {
        size         = BOARD_PROFILE_SIZE_V2;
        crc_coverage = BOARD_PROFILE_CRC_COVERAGE_V2;
    } else {
        return BOARD_PROFILE_ERR_VERSION;
    }
    if (len < size) {
        return BOARD_PROFILE_ERR_SHORT;
    }
    if (blob[BOARD_PROFILE_OFF_LENGTH] != size) {
        return BOARD_PROFILE_ERR_LENGTH;
    }
    if (crc32_ieee(blob, crc_coverage) != load_le32(blob + crc_coverage)) {
        return BOARD_PROFILE_ERR_CRC;
    }

    board_profile_defaults(&decoded);
    decoded.twai_tx_gpio  = blob[BOARD_PROFILE_OFF_TX];
    decoded.twai_rx_gpio  = blob[BOARD_PROFILE_OFF_RX];
    decoded.bitrate       = (board_profile_bitrate_t)blob[BOARD_PROFILE_OFF_BITRATE];
    decoded.hold_low_mask = load_le64(blob + BOARD_PROFILE_OFF_HOLD_MASK);
    if (size == BOARD_PROFILE_SIZE_V2) {
        decoded.hold_high_mask  = load_le64(blob + BOARD_PROFILE_OFF_HOLD_HIGH_MASK);
        decoded.i2c_sda_gpio    = blob[BOARD_PROFILE_OFF_I2C_SDA];
        decoded.i2c_scl_gpio    = blob[BOARD_PROFILE_OFF_I2C_SCL];
        decoded.i2c_addr7       = blob[BOARD_PROFILE_OFF_I2C_ADDR];
        decoded.i2c_write_count = blob[BOARD_PROFILE_OFF_I2C_COUNT];
        /* Unused pairs stay 0xFF: their bytes are ignored on read. */
        for (uint32_t i = 0u; i < decoded.i2c_write_count && i < BOARD_PROFILE_I2C_MAX_WRITES; ++i) {
            decoded.i2c_writes[i].reg   = blob[BOARD_PROFILE_OFF_I2C_WRITES + 2u * i];
            decoded.i2c_writes[i].value = blob[BOARD_PROFILE_OFF_I2C_WRITES + 2u * i + 1u];
        }
    }
    if (board_profile_check(&decoded) != BOARD_PROFILE_OK) {
        return BOARD_PROFILE_ERR_RULE;
    }

    *out = decoded;
    return BOARD_PROFILE_OK;
}

board_profile_status_t board_profile_encode(const board_profile_t *p,
                                            uint8_t out[BOARD_PROFILE_SIZE_V2])
{
    uint8_t rec[BOARD_PROFILE_SIZE_V2];

    if (board_profile_check(p) != BOARD_PROFILE_OK) {
        return BOARD_PROFILE_ERR_RULE;
    }

    memset(rec, 0xFF, sizeof rec);  /* reserved fields and unused write pairs */
    encode_common(p, rec);
    rec[BOARD_PROFILE_OFF_VERSION]   = BOARD_PROFILE_VERSION_V2;
    rec[BOARD_PROFILE_OFF_LENGTH]    = BOARD_PROFILE_SIZE_V2;
    store_le64(rec + BOARD_PROFILE_OFF_HOLD_HIGH_MASK, p->hold_high_mask);
    rec[BOARD_PROFILE_OFF_I2C_SDA]   = p->i2c_sda_gpio;
    rec[BOARD_PROFILE_OFF_I2C_SCL]   = p->i2c_scl_gpio;
    rec[BOARD_PROFILE_OFF_I2C_ADDR]  = p->i2c_addr7;
    rec[BOARD_PROFILE_OFF_I2C_COUNT] = p->i2c_write_count;
    for (uint32_t i = 0u; i < p->i2c_write_count; ++i) {
        rec[BOARD_PROFILE_OFF_I2C_WRITES + 2u * i]      = p->i2c_writes[i].reg;
        rec[BOARD_PROFILE_OFF_I2C_WRITES + 2u * i + 1u] = p->i2c_writes[i].value;
    }
    store_le32(rec + BOARD_PROFILE_OFF_CRC_V2, crc32_ieee(rec, BOARD_PROFILE_CRC_COVERAGE_V2));

    memcpy(out, rec, sizeof rec);
    return BOARD_PROFILE_OK;
}

board_profile_status_t board_profile_encode_v1(const board_profile_t *p,
                                               uint8_t out[BOARD_PROFILE_SIZE_V1])
{
    uint8_t rec[BOARD_PROFILE_SIZE_V1];

    if (board_profile_check(p) != BOARD_PROFILE_OK ||
        p->hold_high_mask != 0u || p->i2c_sda_gpio != BOARD_PROFILE_I2C_NONE) {
        return BOARD_PROFILE_ERR_RULE;
    }

    encode_common(p, rec);
    rec[BOARD_PROFILE_OFF_VERSION] = BOARD_PROFILE_VERSION_V1;
    rec[BOARD_PROFILE_OFF_LENGTH]  = BOARD_PROFILE_SIZE_V1;
    store_le32(rec + BOARD_PROFILE_OFF_CRC_V1, crc32_ieee(rec, BOARD_PROFILE_CRC_COVERAGE_V1));

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
