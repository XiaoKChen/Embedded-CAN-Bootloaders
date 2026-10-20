/**
 * @file    test_board_profile.c
 * @brief   Host-side Unity tests for board_profile_parse / _encode / _check.
 *
 * The golden records are the same bytes ESP-PROG-2-PY-CLI's tests/test_board_profile.py pins for
 * its SOLO presets, so the tool and the updater agree byte for byte.
 *
 * Covers:       Both SOLO golden records (encode and parse), the defaults, every
 *               board_profile_status_t outcome, every field rule, reserved fields and trailing
 *               bytes ignored on read, encode refusing a bad record, the bitrate table.
 * Not covered:  Reading cancfg from flash and driving pins on target.
 *
 * @see     board_profile.h, rules/testing.md
 */

#include <string.h>

#include "unity.h"

#include "board_profile.h"
#include "crc32.h"

/* -----------------------------------------------------------------------------
 * Golden vectors (shared with ESP-PROG-2-PY-CLI)
 * ----------------------------------------------------------------------------- */
static const uint8_t ODU_RECORD[BOARD_PROFILE_SIZE] = {
    0xd1, 0xa7, 0xb0, 0xdf, 0x01, 0x18, 0xff, 0xff, 0x04, 0x05, 0x01, 0xff,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x9e, 0x78, 0x3a,
};
static const uint8_t IDU_RECORD[BOARD_PROFILE_SIZE] = {
    0xd1, 0xa7, 0xb0, 0xdf, 0x01, 0x18, 0xff, 0xff, 0x06, 0x07, 0x01, 0xff,
    0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xcb, 0xa2, 0xca, 0x5d,
};

static const board_profile_t ODU_PROFILE = {4u, 5u, BOARD_PROFILE_BITRATE_250K, 0ull};
static const board_profile_t IDU_PROFILE = {6u, 7u, BOARD_PROFILE_BITRATE_250K, 1ull << 3};

static uint8_t s_rec[BOARD_PROFILE_SIZE + 8u];

/* Re-seal s_rec's crc32 after a field edit, so only the rule under test fails. */
static void reseal(void)
{
    uint32_t crc = crc32_ieee(s_rec, BOARD_PROFILE_CRC_COVERAGE);
    s_rec[BOARD_PROFILE_OFF_CRC + 0] = (uint8_t)(crc & 0xFFu);
    s_rec[BOARD_PROFILE_OFF_CRC + 1] = (uint8_t)((crc >> 8) & 0xFFu);
    s_rec[BOARD_PROFILE_OFF_CRC + 2] = (uint8_t)((crc >> 16) & 0xFFu);
    s_rec[BOARD_PROFILE_OFF_CRC + 3] = (uint8_t)((crc >> 24) & 0xFFu);
}

static void assert_defaults(const board_profile_t *p)
{
    TEST_ASSERT_EQUAL_UINT8(BOARD_PROFILE_DEFAULT_TX_GPIO, p->twai_tx_gpio);
    TEST_ASSERT_EQUAL_UINT8(BOARD_PROFILE_DEFAULT_RX_GPIO, p->twai_rx_gpio);
    TEST_ASSERT_EQUAL_INT(BOARD_PROFILE_DEFAULT_BITRATE, p->bitrate);
    TEST_ASSERT_TRUE(p->hold_low_mask == BOARD_PROFILE_DEFAULT_HOLD);
}

static void assert_profile(const board_profile_t *want, const board_profile_t *got)
{
    TEST_ASSERT_EQUAL_UINT8(want->twai_tx_gpio, got->twai_tx_gpio);
    TEST_ASSERT_EQUAL_UINT8(want->twai_rx_gpio, got->twai_rx_gpio);
    TEST_ASSERT_EQUAL_INT(want->bitrate, got->bitrate);
    TEST_ASSERT_TRUE(want->hold_low_mask == got->hold_low_mask);
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
    memcpy(s_rec, IDU_RECORD, sizeof IDU_RECORD);
}

void tearDown(void) {}

/* -----------------------------------------------------------------------------
 * Golden vectors and defaults
 * ----------------------------------------------------------------------------- */
static void test_encode_matches_golden_vectors(void)
{
    uint8_t out[BOARD_PROFILE_SIZE];
    TEST_ASSERT_EQUAL_INT(BOARD_PROFILE_OK, board_profile_encode(&ODU_PROFILE, out));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(ODU_RECORD, out, BOARD_PROFILE_SIZE);
    TEST_ASSERT_EQUAL_INT(BOARD_PROFILE_OK, board_profile_encode(&IDU_PROFILE, out));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(IDU_RECORD, out, BOARD_PROFILE_SIZE);
}

static void test_parse_golden_vectors(void)
{
    board_profile_t p;
    TEST_ASSERT_EQUAL_INT(BOARD_PROFILE_OK, board_profile_parse(ODU_RECORD, sizeof ODU_RECORD, &p));
    assert_profile(&ODU_PROFILE, &p);
    TEST_ASSERT_EQUAL_INT(BOARD_PROFILE_OK, board_profile_parse(IDU_RECORD, sizeof IDU_RECORD, &p));
    assert_profile(&IDU_PROFILE, &p);
}

static void test_defaults_are_the_odu_wiring(void)
{
    board_profile_t p;
    board_profile_defaults(&p);
    assert_profile(&ODU_PROFILE, &p);
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
    TEST_ASSERT_EQUAL_INT(BOARD_PROFILE_ERR_SHORT,
                          board_profile_parse(s_rec, BOARD_PROFILE_SIZE - 1u, &p));
    assert_defaults(&p);
}

static void test_version_mismatch(void)
{
    board_profile_t p;
    s_rec[BOARD_PROFILE_OFF_VERSION] = 2u;
    reseal();
    TEST_ASSERT_EQUAL_INT(BOARD_PROFILE_ERR_VERSION, board_profile_parse(s_rec, BOARD_PROFILE_SIZE, &p));
    assert_defaults(&p);
}

static void test_length_mismatch(void)
{
    board_profile_t p;
    s_rec[BOARD_PROFILE_OFF_LENGTH] = 28u;
    reseal();
    TEST_ASSERT_EQUAL_INT(BOARD_PROFILE_ERR_LENGTH, board_profile_parse(s_rec, BOARD_PROFILE_SIZE, &p));
    assert_defaults(&p);
}

static void test_bad_crc(void)
{
    board_profile_t p;
    s_rec[BOARD_PROFILE_OFF_CRC] ^= 0xFFu;
    TEST_ASSERT_EQUAL_INT(BOARD_PROFILE_ERR_CRC, board_profile_parse(s_rec, BOARD_PROFILE_SIZE, &p));
    assert_defaults(&p);
}

/* -----------------------------------------------------------------------------
 * Field rules
 * ----------------------------------------------------------------------------- */
static void test_tx_out_of_range(void)       { s_rec[BOARD_PROFILE_OFF_TX] = 49u; assert_rule_rejected(); }
static void test_rx_out_of_range(void)       { s_rec[BOARD_PROFILE_OFF_RX] = 49u; assert_rule_rejected(); }
static void test_tx_refused_usb(void)        { s_rec[BOARD_PROFILE_OFF_TX] = 19u; assert_rule_rejected(); }
static void test_rx_refused_flash(void)      { s_rec[BOARD_PROFILE_OFF_RX] = 32u; assert_rule_rejected(); }
static void test_tx_refused_strap(void)      { s_rec[BOARD_PROFILE_OFF_TX] = 0u;  assert_rule_rejected(); }
static void test_bitrate_unknown(void)       { s_rec[BOARD_PROFILE_OFF_BITRATE] = 4u; assert_rule_rejected(); }

static void test_tx_equals_rx(void)
{
    s_rec[BOARD_PROFILE_OFF_RX]        = 6u;
    s_rec[BOARD_PROFILE_OFF_HOLD_MASK] = 0u;
    assert_rule_rejected();
}

static void test_hold_refused_pin(void)
{
    s_rec[BOARD_PROFILE_OFF_HOLD_MASK] = 0x01u;  /* GPIO0 */
    assert_rule_rejected();
}

static void test_hold_twai_pin(void)
{
    s_rec[BOARD_PROFILE_OFF_HOLD_MASK] = 0x40u;  /* GPIO6 = TX */
    assert_rule_rejected();
}

static void test_hold_above_gpio48(void)
{
    s_rec[BOARD_PROFILE_OFF_HOLD_MASK + 6] = 0x02u;  /* bit 49 */
    assert_rule_rejected();
}

static void test_highest_pins_accepted(void)
{
    board_profile_t want = {21u, 47u, BOARD_PROFILE_BITRATE_1M, (1ull << 1) | (1ull << 48)};
    board_profile_t got;
    uint8_t rec[BOARD_PROFILE_SIZE];
    TEST_ASSERT_EQUAL_INT(BOARD_PROFILE_OK, board_profile_encode(&want, rec));
    TEST_ASSERT_EQUAL_INT(BOARD_PROFILE_OK, board_profile_parse(rec, sizeof rec, &got));
    assert_profile(&want, &got);
}

/* -----------------------------------------------------------------------------
 * Reader leniency and writer strictness
 * ----------------------------------------------------------------------------- */
static void test_reserved_fields_ignored(void)
{
    board_profile_t p;
    s_rec[BOARD_PROFILE_OFF_RESERVED16]     = 0x12u;
    s_rec[BOARD_PROFILE_OFF_RESERVED16 + 1] = 0x34u;
    s_rec[BOARD_PROFILE_OFF_RESERVED8]      = 0x00u;
    reseal();
    TEST_ASSERT_EQUAL_INT(BOARD_PROFILE_OK, board_profile_parse(s_rec, BOARD_PROFILE_SIZE, &p));
    assert_profile(&IDU_PROFILE, &p);
}

static void test_trailing_bytes_ignored(void)
{
    board_profile_t p;
    memset(s_rec + BOARD_PROFILE_SIZE, 0x00, 8u);
    TEST_ASSERT_EQUAL_INT(BOARD_PROFILE_OK, board_profile_parse(s_rec, sizeof s_rec, &p));
    assert_profile(&IDU_PROFILE, &p);
}

static void test_encode_refuses_bad_record(void)
{
    board_profile_t bad = {6u, 7u, BOARD_PROFILE_BITRATE_250K, 1ull << 6};
    uint8_t out[BOARD_PROFILE_SIZE];
    memset(out, 0xAA, sizeof out);
    TEST_ASSERT_EQUAL_INT(BOARD_PROFILE_ERR_RULE, board_profile_encode(&bad, out));
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
    UNITY_BEGIN();
    RUN_TEST(test_encode_matches_golden_vectors);
    RUN_TEST(test_parse_golden_vectors);
    RUN_TEST(test_defaults_are_the_odu_wiring);
    RUN_TEST(test_erased_flash_is_magic_error);
    RUN_TEST(test_short_buffer);
    RUN_TEST(test_version_mismatch);
    RUN_TEST(test_length_mismatch);
    RUN_TEST(test_bad_crc);
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
    RUN_TEST(test_highest_pins_accepted);
    RUN_TEST(test_reserved_fields_ignored);
    RUN_TEST(test_trailing_bytes_ignored);
    RUN_TEST(test_encode_refuses_bad_record);
    RUN_TEST(test_bitrate_table);
    return UNITY_END();
}
