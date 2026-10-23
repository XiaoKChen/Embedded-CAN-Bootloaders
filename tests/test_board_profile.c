/**
 * @file    test_board_profile.c
 * @brief   Host-side Unity tests for board_profile_parse / _encode / _encode_v1 / _check.
 *
 * The golden records are the same bytes ESP-PROG-2-PY-CLI's tests/test_board_profile.py pins for
 * its SOLO presets, so the tool and the updater agree byte for byte: the v1 records inline below,
 * the v2 records in tests/vectors/board_profile_v2.txt.
 *
 * Covers:       v1 and v2 golden records (encode and parse), v1 read through a v2-sized buffer,
 *               the defaults, every board_profile_status_t outcome for both versions, every v1
 *               field rule, the v2 hold-HIGH and I2C rules, reserved fields, unused write pairs
 *               and trailing bytes ignored on read, both encoders refusing a bad record, the v1
 *               encoder refusing v2-only fields, the bitrate table.
 * Not covered:  Reading cancfg from flash and driving pins on target.
 *
 * @see     board_profile.h, rules/testing.md
 */

#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "board_profile.h"
#include "crc32.h"

#ifndef BOARD_PROFILE_VECTORS
#error "BOARD_PROFILE_VECTORS must name tests/vectors/board_profile_v2.txt"
#endif

/* -----------------------------------------------------------------------------
 * Golden vectors (shared with ESP-PROG-2-PY-CLI)
 * ----------------------------------------------------------------------------- */
static const uint8_t ODU_V1_RECORD[BOARD_PROFILE_SIZE_V1] = {
    0xd1, 0xa7, 0xb0, 0xdf, 0x01, 0x18, 0xff, 0xff, 0x04, 0x05, 0x01, 0xff,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x9e, 0x78, 0x3a,
};
static const uint8_t IDU_V1_RECORD[BOARD_PROFILE_SIZE_V1] = {
    0xd1, 0xa7, 0xb0, 0xdf, 0x01, 0x18, 0xff, 0xff, 0x06, 0x07, 0x01, 0xff,
    0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xcb, 0xa2, 0xca, 0x5d,
};
static uint8_t s_odu_v2[BOARD_PROFILE_SIZE_V2];  /* loaded from BOARD_PROFILE_VECTORS */
static uint8_t s_idu_v2[BOARD_PROFILE_SIZE_V2];

#define GPIO(n) (1ull << (n))

static const uint64_t ODU_HOLD_LOW = GPIO(1) | GPIO(2) | GPIO(6) | GPIO(17) | GPIO(18) |
                                     GPIO(33) | GPIO(34) | GPIO(35) | GPIO(47) | GPIO(48);

static uint8_t s_rec[BOARD_PROFILE_SIZE_V2 + 8u];

static board_profile_t v1_profile(uint8_t tx, uint8_t rx, uint64_t hold_low)
{
    board_profile_t p;
    board_profile_defaults(&p);
    p.twai_tx_gpio  = tx;
    p.twai_rx_gpio  = rx;
    p.hold_low_mask = hold_low;
    return p;
}

static board_profile_t odu_v2_profile(void)
{
    board_profile_t p = v1_profile(4u, 5u, ODU_HOLD_LOW);
    p.hold_high_mask  = GPIO(21);
    p.i2c_sda_gpio    = 8u;
    p.i2c_scl_gpio    = 9u;
    p.i2c_addr7       = 0x2Cu;
    p.i2c_write_count = 5u;
    for (uint8_t i = 0u; i < 5u; ++i) {
        p.i2c_writes[i].reg   = (uint8_t)(0x30u + 0x10u * i);  /* Fan 1..5 Setting */
        p.i2c_writes[i].value = 0x00u;
    }
    return p;
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

/* Decode exactly BOARD_PROFILE_SIZE_V2 bytes of lowercase hex ending the line; 0 on any error. */
static int decode_hex(const char *hex, uint8_t *dst)
{
    for (size_t i = 0u; i < BOARD_PROFILE_SIZE_V2; ++i) {
        const int hi = hex_nibble(hex[2u * i]);
        const int lo = hex_nibble(hex[2u * i + 1u]);
        if (hi < 0 || lo < 0) {
            return 0;
        }
        dst[i] = (uint8_t)((hi << 4) | lo);
    }
    const char end = hex[2u * BOARD_PROFILE_SIZE_V2];
    return end == '\0' || end == '\n' || end == '\r';
}

static void load_vectors(void)
{
    FILE *f = fopen(BOARD_PROFILE_VECTORS, "r");
    char  line[256];
    int   found = 0;

    if (f == NULL) {
        return;
    }
    while (fgets(line, sizeof line, f) != NULL) {
        if (strncmp(line, "odu_v2 ", 7u) == 0) {
            found += decode_hex(line + 7, s_odu_v2);
        } else if (strncmp(line, "idu_v2 ", 7u) == 0) {
            found += decode_hex(line + 7, s_idu_v2);
        }
    }
    fclose(f);
    if (found != 2) {
        memset(s_odu_v2, 0, sizeof s_odu_v2);  /* fails every v2 golden test loudly */
    }
}

/* Re-seal s_rec's crc32 after a field edit, so only the rule under test fails. */
static void reseal_at(uint32_t coverage)
{
    uint32_t crc = crc32_ieee(s_rec, coverage);
    s_rec[coverage + 0] = (uint8_t)(crc & 0xFFu);
    s_rec[coverage + 1] = (uint8_t)((crc >> 8) & 0xFFu);
    s_rec[coverage + 2] = (uint8_t)((crc >> 16) & 0xFFu);
    s_rec[coverage + 3] = (uint8_t)((crc >> 24) & 0xFFu);
}

static void reseal(void)
{
    reseal_at(s_rec[BOARD_PROFILE_OFF_VERSION] == BOARD_PROFILE_VERSION_V1
                  ? BOARD_PROFILE_CRC_COVERAGE_V1 : BOARD_PROFILE_CRC_COVERAGE_V2);
}

static void use_v1_record(void)
{
    memset(s_rec, 0xFF, sizeof s_rec);
    memcpy(s_rec, IDU_V1_RECORD, sizeof IDU_V1_RECORD);
}

static void assert_profile(const board_profile_t *want, const board_profile_t *got)
{
    TEST_ASSERT_EQUAL_UINT8(want->twai_tx_gpio, got->twai_tx_gpio);
    TEST_ASSERT_EQUAL_UINT8(want->twai_rx_gpio, got->twai_rx_gpio);
    TEST_ASSERT_EQUAL_INT(want->bitrate, got->bitrate);
    TEST_ASSERT_TRUE(want->hold_low_mask == got->hold_low_mask);
    TEST_ASSERT_TRUE(want->hold_high_mask == got->hold_high_mask);
    TEST_ASSERT_EQUAL_UINT8(want->i2c_sda_gpio, got->i2c_sda_gpio);
    TEST_ASSERT_EQUAL_UINT8(want->i2c_scl_gpio, got->i2c_scl_gpio);
    TEST_ASSERT_EQUAL_UINT8(want->i2c_addr7, got->i2c_addr7);
    TEST_ASSERT_EQUAL_UINT8(want->i2c_write_count, got->i2c_write_count);
    TEST_ASSERT_EQUAL_UINT8_ARRAY((const uint8_t *)want->i2c_writes,
                                  (const uint8_t *)got->i2c_writes, sizeof want->i2c_writes);
}

static void assert_defaults(const board_profile_t *p)
{
    board_profile_t want;
    board_profile_defaults(&want);
    assert_profile(&want, p);
}

/* Parse s_rec after a field edit + reseal; expect the rule failure and the defaults. */
static void assert_rule_rejected(void)
{
    board_profile_t p;
    reseal();
    TEST_ASSERT_EQUAL_INT(BOARD_PROFILE_ERR_RULE, board_profile_parse(s_rec, BOARD_PROFILE_SIZE, &p));
    assert_defaults(&p);
}

void setUp(void)
{
    memset(s_rec, 0xFF, sizeof s_rec);
    memcpy(s_rec, s_odu_v2, sizeof s_odu_v2);
}

void tearDown(void) {}

/* -----------------------------------------------------------------------------
 * Golden vectors and defaults
 * ----------------------------------------------------------------------------- */
static void test_v2_encode_matches_golden_vectors(void)
{
    const board_profile_t odu = odu_v2_profile();
    const board_profile_t idu = v1_profile(6u, 7u, GPIO(3));
    uint8_t out[BOARD_PROFILE_SIZE_V2];
    TEST_ASSERT_EQUAL_INT(BOARD_PROFILE_OK, board_profile_encode(&odu, out));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(s_odu_v2, out, BOARD_PROFILE_SIZE_V2);
    TEST_ASSERT_EQUAL_INT(BOARD_PROFILE_OK, board_profile_encode(&idu, out));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(s_idu_v2, out, BOARD_PROFILE_SIZE_V2);
}

static void test_v2_parse_golden_vectors(void)
{
    const board_profile_t odu = odu_v2_profile();
    const board_profile_t idu = v1_profile(6u, 7u, GPIO(3));
    board_profile_t p;
    TEST_ASSERT_EQUAL_INT(BOARD_PROFILE_OK, board_profile_parse(s_odu_v2, sizeof s_odu_v2, &p));
    assert_profile(&odu, &p);
    TEST_ASSERT_EQUAL_INT(BOARD_PROFILE_OK, board_profile_parse(s_idu_v2, sizeof s_idu_v2, &p));
    assert_profile(&idu, &p);
}

static void test_v1_encode_matches_golden_vectors(void)
{
    const board_profile_t odu = v1_profile(4u, 5u, 0u);
    const board_profile_t idu = v1_profile(6u, 7u, GPIO(3));
    uint8_t out[BOARD_PROFILE_SIZE_V1];
    TEST_ASSERT_EQUAL_INT(BOARD_PROFILE_OK, board_profile_encode_v1(&odu, out));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(ODU_V1_RECORD, out, BOARD_PROFILE_SIZE_V1);
    TEST_ASSERT_EQUAL_INT(BOARD_PROFILE_OK, board_profile_encode_v1(&idu, out));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(IDU_V1_RECORD, out, BOARD_PROFILE_SIZE_V1);
}

static void test_v1_parse_golden_vectors(void)
{
    const board_profile_t odu = v1_profile(4u, 5u, 0u);
    const board_profile_t idu = v1_profile(6u, 7u, GPIO(3));
    board_profile_t p;
    TEST_ASSERT_EQUAL_INT(BOARD_PROFILE_OK, board_profile_parse(ODU_V1_RECORD, sizeof ODU_V1_RECORD, &p));
    assert_profile(&odu, &p);
    TEST_ASSERT_EQUAL_INT(BOARD_PROFILE_OK, board_profile_parse(IDU_V1_RECORD, sizeof IDU_V1_RECORD, &p));
    assert_profile(&idu, &p);
}

/* Readers fetch BOARD_PROFILE_SIZE bytes; a v1 record is followed by erased flash. */
static void test_v1_parses_from_full_read(void)
{
    const board_profile_t idu = v1_profile(6u, 7u, GPIO(3));
    board_profile_t p;
    use_v1_record();
    TEST_ASSERT_EQUAL_INT(BOARD_PROFILE_OK, board_profile_parse(s_rec, BOARD_PROFILE_SIZE, &p));
    assert_profile(&idu, &p);
}

static void test_defaults_are_the_odu_wiring(void)
{
    const board_profile_t odu = v1_profile(4u, 5u, 0u);
    board_profile_t p;
    board_profile_defaults(&p);
    assert_profile(&odu, &p);
    TEST_ASSERT_EQUAL_UINT8(BOARD_PROFILE_I2C_NONE, p.i2c_sda_gpio);
    TEST_ASSERT_EQUAL_UINT8(0u, p.i2c_write_count);
}

/* -----------------------------------------------------------------------------
 * Header and CRC failures fall back to the defaults
 * ----------------------------------------------------------------------------- */
static void test_erased_flash_is_magic_error(void)
{
    board_profile_t p;
    memset(s_rec, 0xFF, sizeof s_rec);
    TEST_ASSERT_EQUAL_INT(BOARD_PROFILE_ERR_MAGIC, board_profile_parse(s_rec, BOARD_PROFILE_SIZE, &p));
    assert_defaults(&p);
}

static void test_short_buffer(void)
{
    board_profile_t p;
    use_v1_record();
    TEST_ASSERT_EQUAL_INT(BOARD_PROFILE_ERR_SHORT,
                          board_profile_parse(s_rec, BOARD_PROFILE_SIZE_V1 - 1u, &p));
    assert_defaults(&p);
}

static void test_v2_short_buffer(void)
{
    board_profile_t p;
    TEST_ASSERT_EQUAL_INT(BOARD_PROFILE_ERR_SHORT,
                          board_profile_parse(s_rec, BOARD_PROFILE_SIZE_V2 - 1u, &p));
    assert_defaults(&p);
}

static void test_version_unknown(void)
{
    board_profile_t p;
    s_rec[BOARD_PROFILE_OFF_VERSION] = 3u;
    reseal_at(BOARD_PROFILE_CRC_COVERAGE_V2);
    TEST_ASSERT_EQUAL_INT(BOARD_PROFILE_ERR_VERSION, board_profile_parse(s_rec, BOARD_PROFILE_SIZE, &p));
    assert_defaults(&p);
}

static void test_v1_length_mismatch(void)
{
    board_profile_t p;
    use_v1_record();
    s_rec[BOARD_PROFILE_OFF_LENGTH] = 28u;
    reseal();
    TEST_ASSERT_EQUAL_INT(BOARD_PROFILE_ERR_LENGTH, board_profile_parse(s_rec, BOARD_PROFILE_SIZE, &p));
    assert_defaults(&p);
}

/* A v1 body relabelled v2 (or the reverse) never decodes. */
static void test_v2_length_mismatch(void)
{
    board_profile_t p;
    s_rec[BOARD_PROFILE_OFF_LENGTH] = BOARD_PROFILE_SIZE_V1;
    reseal();
    TEST_ASSERT_EQUAL_INT(BOARD_PROFILE_ERR_LENGTH, board_profile_parse(s_rec, BOARD_PROFILE_SIZE, &p));
    assert_defaults(&p);
}

static void test_v1_bad_crc(void)
{
    board_profile_t p;
    use_v1_record();
    s_rec[BOARD_PROFILE_OFF_CRC_V1] ^= 0xFFu;
    TEST_ASSERT_EQUAL_INT(BOARD_PROFILE_ERR_CRC, board_profile_parse(s_rec, BOARD_PROFILE_SIZE, &p));
    assert_defaults(&p);
}

static void test_v2_bad_crc(void)
{
    board_profile_t p;
    s_rec[BOARD_PROFILE_OFF_CRC_V2 + 3] ^= 0x01u;
    TEST_ASSERT_EQUAL_INT(BOARD_PROFILE_ERR_CRC, board_profile_parse(s_rec, BOARD_PROFILE_SIZE, &p));
    assert_defaults(&p);
}

static void test_v2_crc_covers_write_pairs(void)
{
    board_profile_t p;
    s_rec[BOARD_PROFILE_OFF_I2C_WRITES + 1] = 0xFFu;  /* Fan 1 to full scale, not resealed */
    TEST_ASSERT_EQUAL_INT(BOARD_PROFILE_ERR_CRC, board_profile_parse(s_rec, BOARD_PROFILE_SIZE, &p));
    assert_defaults(&p);
}

/* -----------------------------------------------------------------------------
 * v1 field rules (shared by v2)
 * ----------------------------------------------------------------------------- */
static void test_tx_out_of_range(void)  { use_v1_record(); s_rec[BOARD_PROFILE_OFF_TX] = 49u; assert_rule_rejected(); }
static void test_rx_out_of_range(void)  { use_v1_record(); s_rec[BOARD_PROFILE_OFF_RX] = 49u; assert_rule_rejected(); }
static void test_tx_refused_usb(void)   { use_v1_record(); s_rec[BOARD_PROFILE_OFF_TX] = 19u; assert_rule_rejected(); }
static void test_rx_refused_flash(void) { use_v1_record(); s_rec[BOARD_PROFILE_OFF_RX] = 32u; assert_rule_rejected(); }
static void test_tx_refused_strap(void) { use_v1_record(); s_rec[BOARD_PROFILE_OFF_TX] = 0u;  assert_rule_rejected(); }
static void test_bitrate_unknown(void)  { use_v1_record(); s_rec[BOARD_PROFILE_OFF_BITRATE] = 4u; assert_rule_rejected(); }

static void test_tx_equals_rx(void)
{
    use_v1_record();
    s_rec[BOARD_PROFILE_OFF_RX]        = 6u;
    s_rec[BOARD_PROFILE_OFF_HOLD_MASK] = 0u;
    assert_rule_rejected();
}

static void test_hold_refused_pin(void)
{
    use_v1_record();
    s_rec[BOARD_PROFILE_OFF_HOLD_MASK] = 0x01u;  /* GPIO0 */
    assert_rule_rejected();
}

static void test_hold_twai_pin(void)
{
    use_v1_record();
    s_rec[BOARD_PROFILE_OFF_HOLD_MASK] = 0x40u;  /* GPIO6 = TX */
    assert_rule_rejected();
}

static void test_hold_above_gpio48(void)
{
    use_v1_record();
    s_rec[BOARD_PROFILE_OFF_HOLD_MASK + 6] = 0x02u;  /* bit 49 */
    assert_rule_rejected();
}

static void test_v2_twai_rule_still_applies(void)
{
    s_rec[BOARD_PROFILE_OFF_TX] = 20u;  /* USB D+ */
    assert_rule_rejected();
}

/* -----------------------------------------------------------------------------
 * v2 hold-HIGH rules (ODU base record: TX 4, RX 5, hold HIGH GPIO21)
 * ----------------------------------------------------------------------------- */
static void test_hold_high_refused_pin(void)
{
    s_rec[BOARD_PROFILE_OFF_HOLD_HIGH_MASK + 2] |= 0x08u;  /* GPIO19 */
    assert_rule_rejected();
}

static void test_hold_high_twai_pin(void)
{
    s_rec[BOARD_PROFILE_OFF_HOLD_HIGH_MASK] = 0x10u;  /* GPIO4 = TX */
    assert_rule_rejected();
}

static void test_hold_high_above_gpio48(void)
{
    s_rec[BOARD_PROFILE_OFF_HOLD_HIGH_MASK + 7] = 0x80u;  /* bit 63 */
    assert_rule_rejected();
}

static void test_hold_high_overlaps_hold_low(void)
{
    s_rec[BOARD_PROFILE_OFF_HOLD_HIGH_MASK] = 0x02u;  /* GPIO1, also held LOW */
    assert_rule_rejected();
}

/* -----------------------------------------------------------------------------
 * v2 I2C rules (ODU base record: SDA 8, SCL 9, addr 0x2C, 5 writes)
 * ----------------------------------------------------------------------------- */
static void test_i2c_none_with_scl(void)
{
    s_rec[BOARD_PROFILE_OFF_I2C_SDA]   = BOARD_PROFILE_I2C_NONE;
    s_rec[BOARD_PROFILE_OFF_I2C_ADDR]  = BOARD_PROFILE_I2C_NONE;
    s_rec[BOARD_PROFILE_OFF_I2C_COUNT] = 0u;
    assert_rule_rejected();  /* SCL still 9 */
}

static void test_i2c_none_with_addr(void)
{
    s_rec[BOARD_PROFILE_OFF_I2C_SDA]   = BOARD_PROFILE_I2C_NONE;
    s_rec[BOARD_PROFILE_OFF_I2C_SCL]   = BOARD_PROFILE_I2C_NONE;
    s_rec[BOARD_PROFILE_OFF_I2C_COUNT] = 0u;
    assert_rule_rejected();  /* addr still 0x2C */
}

static void test_i2c_none_with_writes(void)
{
    s_rec[BOARD_PROFILE_OFF_I2C_SDA]  = BOARD_PROFILE_I2C_NONE;
    s_rec[BOARD_PROFILE_OFF_I2C_SCL]  = BOARD_PROFILE_I2C_NONE;
    s_rec[BOARD_PROFILE_OFF_I2C_ADDR] = BOARD_PROFILE_I2C_NONE;
    assert_rule_rejected();  /* count still 5 */
}

static void test_i2c_none_accepted(void)
{
    board_profile_t want = odu_v2_profile();
    board_profile_t p;
    s_rec[BOARD_PROFILE_OFF_I2C_SDA]   = BOARD_PROFILE_I2C_NONE;
    s_rec[BOARD_PROFILE_OFF_I2C_SCL]   = BOARD_PROFILE_I2C_NONE;
    s_rec[BOARD_PROFILE_OFF_I2C_ADDR]  = BOARD_PROFILE_I2C_NONE;
    s_rec[BOARD_PROFILE_OFF_I2C_COUNT] = 0u;
    reseal();
    TEST_ASSERT_EQUAL_INT(BOARD_PROFILE_OK, board_profile_parse(s_rec, BOARD_PROFILE_SIZE, &p));
    want.i2c_sda_gpio = want.i2c_scl_gpio = want.i2c_addr7 = BOARD_PROFILE_I2C_NONE;
    want.i2c_write_count = 0u;
    memset(want.i2c_writes, 0xFF, sizeof want.i2c_writes);  /* stale pairs are not decoded */
    assert_profile(&want, &p);
}

static void test_i2c_sda_equals_scl(void)  { s_rec[BOARD_PROFILE_OFF_I2C_SCL] = 8u;  assert_rule_rejected(); }
static void test_i2c_sda_refused(void)     { s_rec[BOARD_PROFILE_OFF_I2C_SDA] = 26u; assert_rule_rejected(); }
static void test_i2c_scl_out_of_range(void){ s_rec[BOARD_PROFILE_OFF_I2C_SCL] = 49u; assert_rule_rejected(); }
static void test_i2c_sda_twai_pin(void)    { s_rec[BOARD_PROFILE_OFF_I2C_SDA] = 5u;  assert_rule_rejected(); }
static void test_i2c_scl_held_low(void)    { s_rec[BOARD_PROFILE_OFF_I2C_SCL] = 17u; assert_rule_rejected(); }
static void test_i2c_sda_held_high(void)   { s_rec[BOARD_PROFILE_OFF_I2C_SDA] = 21u; assert_rule_rejected(); }
static void test_i2c_addr_below_range(void){ s_rec[BOARD_PROFILE_OFF_I2C_ADDR] = 0x07u; assert_rule_rejected(); }
static void test_i2c_addr_above_range(void){ s_rec[BOARD_PROFILE_OFF_I2C_ADDR] = 0x78u; assert_rule_rejected(); }
static void test_i2c_count_zero(void)      { s_rec[BOARD_PROFILE_OFF_I2C_COUNT] = 0u; assert_rule_rejected(); }
static void test_i2c_count_above_8(void)   { s_rec[BOARD_PROFILE_OFF_I2C_COUNT] = 9u; assert_rule_rejected(); }

static void test_i2c_address_bounds_accepted(void)
{
    board_profile_t p;
    s_rec[BOARD_PROFILE_OFF_I2C_ADDR] = BOARD_PROFILE_I2C_ADDR_MIN;
    reseal();
    TEST_ASSERT_EQUAL_INT(BOARD_PROFILE_OK, board_profile_parse(s_rec, BOARD_PROFILE_SIZE, &p));
    s_rec[BOARD_PROFILE_OFF_I2C_ADDR] = BOARD_PROFILE_I2C_ADDR_MAX;
    reseal();
    TEST_ASSERT_EQUAL_INT(BOARD_PROFILE_OK, board_profile_parse(s_rec, BOARD_PROFILE_SIZE, &p));
}

static void test_highest_pins_accepted(void)
{
    board_profile_t want = v1_profile(21u, 47u, GPIO(1) | GPIO(48));
    board_profile_t got;
    uint8_t rec[BOARD_PROFILE_SIZE_V2];
    want.bitrate         = BOARD_PROFILE_BITRATE_1M;
    want.hold_high_mask  = GPIO(2) | GPIO(46);
    want.i2c_sda_gpio    = 45u;
    want.i2c_scl_gpio    = 44u;
    want.i2c_addr7       = 0x77u;
    want.i2c_write_count = BOARD_PROFILE_I2C_MAX_WRITES;
    for (uint8_t i = 0u; i < BOARD_PROFILE_I2C_MAX_WRITES; ++i) {
        want.i2c_writes[i].reg   = i;
        want.i2c_writes[i].value = (uint8_t)(0xF0u + i);
    }
    TEST_ASSERT_EQUAL_INT(BOARD_PROFILE_OK, board_profile_encode(&want, rec));
    TEST_ASSERT_EQUAL_INT(BOARD_PROFILE_OK, board_profile_parse(rec, sizeof rec, &got));
    assert_profile(&want, &got);
}

/* -----------------------------------------------------------------------------
 * Reader leniency and writer strictness
 * ----------------------------------------------------------------------------- */
static void test_v1_reserved_fields_ignored(void)
{
    const board_profile_t idu = v1_profile(6u, 7u, GPIO(3));
    board_profile_t p;
    use_v1_record();
    s_rec[BOARD_PROFILE_OFF_RESERVED16]     = 0x12u;
    s_rec[BOARD_PROFILE_OFF_RESERVED16 + 1] = 0x34u;
    s_rec[BOARD_PROFILE_OFF_RESERVED8]      = 0x00u;
    reseal();
    TEST_ASSERT_EQUAL_INT(BOARD_PROFILE_OK, board_profile_parse(s_rec, BOARD_PROFILE_SIZE, &p));
    assert_profile(&idu, &p);
}

static void test_v2_reserved_fields_and_unused_pairs_ignored(void)
{
    const board_profile_t odu = odu_v2_profile();
    board_profile_t p;
    s_rec[BOARD_PROFILE_OFF_RESERVED16]   = 0x00u;
    s_rec[BOARD_PROFILE_OFF_RESERVED8]    = 0x00u;
    s_rec[BOARD_PROFILE_OFF_RESERVED32]   = 0x00u;
    s_rec[BOARD_PROFILE_OFF_RESERVED32 + 3] = 0x5Au;
    s_rec[BOARD_PROFILE_OFF_I2C_WRITES + 2u * 5u]      = 0x10u;  /* pair 5: past the count */
    s_rec[BOARD_PROFILE_OFF_I2C_WRITES + 2u * 7u + 1u] = 0x00u;
    reseal();
    TEST_ASSERT_EQUAL_INT(BOARD_PROFILE_OK, board_profile_parse(s_rec, BOARD_PROFILE_SIZE, &p));
    assert_profile(&odu, &p);
}

static void test_trailing_bytes_ignored(void)
{
    const board_profile_t odu = odu_v2_profile();
    board_profile_t p;
    memset(s_rec + BOARD_PROFILE_SIZE_V2, 0x00, 8u);
    TEST_ASSERT_EQUAL_INT(BOARD_PROFILE_OK, board_profile_parse(s_rec, sizeof s_rec, &p));
    assert_profile(&odu, &p);
}

static void test_encode_refuses_bad_record(void)
{
    board_profile_t bad = odu_v2_profile();
    uint8_t out[BOARD_PROFILE_SIZE_V2];
    bad.hold_high_mask |= GPIO(9);  /* the SCL pin */
    memset(out, 0xAA, sizeof out);
    TEST_ASSERT_EQUAL_INT(BOARD_PROFILE_ERR_RULE, board_profile_encode(&bad, out));
    TEST_ASSERT_EACH_EQUAL_UINT8(0xAAu, out, sizeof out);
}

static void test_encode_v1_refuses_bad_record(void)
{
    board_profile_t bad = v1_profile(6u, 7u, GPIO(6));
    uint8_t out[BOARD_PROFILE_SIZE_V1];
    memset(out, 0xAA, sizeof out);
    TEST_ASSERT_EQUAL_INT(BOARD_PROFILE_ERR_RULE, board_profile_encode_v1(&bad, out));
    TEST_ASSERT_EACH_EQUAL_UINT8(0xAAu, out, sizeof out);
}

static void test_encode_v1_refuses_v2_fields(void)
{
    board_profile_t hold_high = v1_profile(6u, 7u, GPIO(3));
    board_profile_t i2c       = odu_v2_profile();
    uint8_t out[BOARD_PROFILE_SIZE_V1];
    hold_high.hold_high_mask = GPIO(21);
    i2c.hold_high_mask       = 0u;
    memset(out, 0xAA, sizeof out);
    TEST_ASSERT_EQUAL_INT(BOARD_PROFILE_ERR_RULE, board_profile_encode_v1(&hold_high, out));
    TEST_ASSERT_EQUAL_INT(BOARD_PROFILE_ERR_RULE, board_profile_encode_v1(&i2c, out));
    TEST_ASSERT_EACH_EQUAL_UINT8(0xAAu, out, sizeof out);
}

static void test_bitrate_table(void)
{
    TEST_ASSERT_EQUAL_UINT32(125000u,  board_profile_bitrate_bps(BOARD_PROFILE_BITRATE_125K));
    TEST_ASSERT_EQUAL_UINT32(250000u,  board_profile_bitrate_bps(BOARD_PROFILE_BITRATE_250K));
    TEST_ASSERT_EQUAL_UINT32(500000u,  board_profile_bitrate_bps(BOARD_PROFILE_BITRATE_500K));
    TEST_ASSERT_EQUAL_UINT32(1000000u, board_profile_bitrate_bps(BOARD_PROFILE_BITRATE_1M));
}

int main(void)
{
    load_vectors();

    UNITY_BEGIN();
    RUN_TEST(test_v2_encode_matches_golden_vectors);
    RUN_TEST(test_v2_parse_golden_vectors);
    RUN_TEST(test_v1_encode_matches_golden_vectors);
    RUN_TEST(test_v1_parse_golden_vectors);
    RUN_TEST(test_v1_parses_from_full_read);
    RUN_TEST(test_defaults_are_the_odu_wiring);
    RUN_TEST(test_erased_flash_is_magic_error);
    RUN_TEST(test_short_buffer);
    RUN_TEST(test_v2_short_buffer);
    RUN_TEST(test_version_unknown);
    RUN_TEST(test_v1_length_mismatch);
    RUN_TEST(test_v2_length_mismatch);
    RUN_TEST(test_v1_bad_crc);
    RUN_TEST(test_v2_bad_crc);
    RUN_TEST(test_v2_crc_covers_write_pairs);
    RUN_TEST(test_tx_out_of_range);
    RUN_TEST(test_rx_out_of_range);
    RUN_TEST(test_tx_refused_usb);
    RUN_TEST(test_rx_refused_flash);
    RUN_TEST(test_tx_refused_strap);
    RUN_TEST(test_bitrate_unknown);
    RUN_TEST(test_tx_equals_rx);
    RUN_TEST(test_hold_refused_pin);
    RUN_TEST(test_hold_twai_pin);
    RUN_TEST(test_hold_above_gpio48);
    RUN_TEST(test_v2_twai_rule_still_applies);
    RUN_TEST(test_hold_high_refused_pin);
    RUN_TEST(test_hold_high_twai_pin);
    RUN_TEST(test_hold_high_above_gpio48);
    RUN_TEST(test_hold_high_overlaps_hold_low);
    RUN_TEST(test_i2c_none_with_scl);
    RUN_TEST(test_i2c_none_with_addr);
    RUN_TEST(test_i2c_none_with_writes);
    RUN_TEST(test_i2c_none_accepted);
    RUN_TEST(test_i2c_sda_equals_scl);
    RUN_TEST(test_i2c_sda_refused);
    RUN_TEST(test_i2c_scl_out_of_range);
    RUN_TEST(test_i2c_sda_twai_pin);
    RUN_TEST(test_i2c_scl_held_low);
    RUN_TEST(test_i2c_sda_held_high);
    RUN_TEST(test_i2c_addr_below_range);
    RUN_TEST(test_i2c_addr_above_range);
    RUN_TEST(test_i2c_count_zero);
    RUN_TEST(test_i2c_count_above_8);
    RUN_TEST(test_i2c_address_bounds_accepted);
    RUN_TEST(test_highest_pins_accepted);
    RUN_TEST(test_v1_reserved_fields_ignored);
    RUN_TEST(test_v2_reserved_fields_and_unused_pairs_ignored);
    RUN_TEST(test_trailing_bytes_ignored);
    RUN_TEST(test_encode_refuses_bad_record);
    RUN_TEST(test_encode_v1_refuses_bad_record);
    RUN_TEST(test_encode_v1_refuses_v2_fields);
    RUN_TEST(test_bitrate_table);
    return UNITY_END();
}
