/**
 * @file    test_dfu_config.c
 * @brief   Host-side Unity tests for dfu_config_parse / dfu_config_can_ids.
 *
 * An 80-byte buffer stands in for the RA4M1 data-flash config block (large enough for both a v1
 * 76-byte and a v2 80-byte record). Pure logic: no hardware, mirrors test_image_validate.c.
 *
 * Covers:       Every dfu_config_status_t outcome (rules/testing.md), plus the empty-string
 *               boundary, both on-disk versions, and the per-device CAN-ID fallbacks.
 * Not covered:  Reading the block from data flash on target.
 *
 * @see     dfu_config.h, rules/testing.md
 */

#include <string.h>

#include "unity.h"

#include "boot_config.h"  /* CAN_ID_HOST_TO_BOOT / CAN_ID_BOOT_TO_HOST defaults */
#include "crc32.h"
#include "dfu_config.h"

/* -----------------------------------------------------------------------------
 * Fixture: a self-consistent valid config block
 * ----------------------------------------------------------------------------- */
static uint8_t s_blob[DFU_CONFIG_SIZE];

static const char TEST_MFR[]  = "Normal Corporation";  /* 18 chars */
static const char TEST_PROD[] = "Widget R4";           /* 9 chars  */

static void store_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void store_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

/* Recompute and store the v1 crc32 (over the first 72 bytes at 0x48). */
static void fix_crc_v1(void)
{
    store_le32(&s_blob[DFU_CONFIG_OFF_CRC_V1],
               crc32_ieee(s_blob, DFU_CONFIG_CRC_COVERAGE_V1));
}

/* Recompute and store the v2 crc32 (over the first 76 bytes at 0x4C). */
static void fix_crc_v2(void)
{
    store_le32(&s_blob[DFU_CONFIG_OFF_CRC_V2],
               crc32_ieee(s_blob, DFU_CONFIG_CRC_COVERAGE_V2));
}

static void build_names(const char *mfr, uint8_t mfr_len,
                        const char *prod, uint8_t prod_len)
{
    memset(s_blob, 0xFF, sizeof(s_blob));  /* erased-flash fill; pads = 0xFF */
    store_le32(&s_blob[DFU_CONFIG_OFF_MAGIC], DFU_CONFIG_MAGIC);
    s_blob[DFU_CONFIG_OFF_MFR_LEN]  = mfr_len;
    s_blob[DFU_CONFIG_OFF_PROD_LEN] = prod_len;
    s_blob[DFU_CONFIG_OFF_RESERVED] = 0xFFu;
    memcpy(&s_blob[DFU_CONFIG_OFF_MFR], mfr, mfr_len);
    memcpy(&s_blob[DFU_CONFIG_OFF_PROD], prod, prod_len);
}

static void build_valid_v1_block(const char *mfr, uint8_t mfr_len,
                                 const char *prod, uint8_t prod_len)
{
    build_names(mfr, mfr_len, prod, prod_len);
    s_blob[DFU_CONFIG_OFF_VERSION] = (uint8_t)DFU_CONFIG_VERSION_V1;
    fix_crc_v1();
}

static void build_valid_v2_block(const char *mfr, uint8_t mfr_len,
                                 const char *prod, uint8_t prod_len,
                                 uint16_t cmd_id, uint16_t reply_id)
{
    build_names(mfr, mfr_len, prod, prod_len);
    s_blob[DFU_CONFIG_OFF_VERSION] = (uint8_t)DFU_CONFIG_VERSION_V2;
    store_le16(&s_blob[DFU_CONFIG_OFF_BOOT_CMD_ID], cmd_id);
    store_le16(&s_blob[DFU_CONFIG_OFF_BOOT_REPLY_ID], reply_id);
    fix_crc_v2();
}

void setUp(void)
{
    build_valid_v1_block(TEST_MFR, (uint8_t)strlen(TEST_MFR),
                         TEST_PROD, (uint8_t)strlen(TEST_PROD));
}
void tearDown(void) {}

/* -----------------------------------------------------------------------------
 * Name parse — one per dfu_config_status_t outcome, plus boundaries (v1)
 * ----------------------------------------------------------------------------- */
static void test_valid_block_returns_strings(void)
{
    const char *mfr = NULL;
    const char *prod = NULL;
    uint8_t mfr_len = 0;
    uint8_t prod_len = 0;

    TEST_ASSERT_EQUAL(DFU_CFG_OK,
        dfu_config_parse(s_blob, &mfr, &mfr_len, &prod, &prod_len));

    TEST_ASSERT_EQUAL_UINT8((uint8_t)strlen(TEST_MFR), mfr_len);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)strlen(TEST_PROD), prod_len);
    /* Out-params point into the blob itself, not a copy. */
    TEST_ASSERT_EQUAL_PTR(&s_blob[DFU_CONFIG_OFF_MFR], mfr);
    TEST_ASSERT_EQUAL_PTR(&s_blob[DFU_CONFIG_OFF_PROD], prod);
    TEST_ASSERT_EQUAL_MEMORY(TEST_MFR, mfr, mfr_len);
    TEST_ASSERT_EQUAL_MEMORY(TEST_PROD, prod, prod_len);
}

static void test_erased_block_fails_magic(void)
{
    const char *mfr, *prod;
    uint8_t mfr_len, prod_len;
    memset(s_blob, 0xFF, sizeof(s_blob));  /* fresh/erased data flash */
    TEST_ASSERT_EQUAL(DFU_CFG_ERR_MAGIC,
        dfu_config_parse(s_blob, &mfr, &mfr_len, &prod, &prod_len));
}

static void test_bad_magic_fails(void)
{
    const char *mfr, *prod;
    uint8_t mfr_len, prod_len;
    s_blob[DFU_CONFIG_OFF_MAGIC] ^= 0x01u;  /* corrupt magic (CRC not fixed) */
    TEST_ASSERT_EQUAL(DFU_CFG_ERR_MAGIC,
        dfu_config_parse(s_blob, &mfr, &mfr_len, &prod, &prod_len));
}

static void test_unknown_version_fails(void)
{
    const char *mfr, *prod;
    uint8_t mfr_len, prod_len;
    s_blob[DFU_CONFIG_OFF_VERSION] = 0x03u;  /* neither v1 nor v2 */
    fix_crc_v1();  /* version is checked before CRC; keep CRC honest anyway */
    TEST_ASSERT_EQUAL(DFU_CFG_ERR_VERSION,
        dfu_config_parse(s_blob, &mfr, &mfr_len, &prod, &prod_len));
}

static void test_mfr_len_over_max_fails(void)
{
    const char *mfr, *prod;
    uint8_t mfr_len, prod_len;
    s_blob[DFU_CONFIG_OFF_MFR_LEN] = (uint8_t)(DFU_CONFIG_STR_MAX + 1u);
    fix_crc_v1();
    TEST_ASSERT_EQUAL(DFU_CFG_ERR_LEN,
        dfu_config_parse(s_blob, &mfr, &mfr_len, &prod, &prod_len));
}

static void test_prod_len_over_max_fails(void)
{
    const char *mfr, *prod;
    uint8_t mfr_len, prod_len;
    s_blob[DFU_CONFIG_OFF_PROD_LEN] = (uint8_t)(DFU_CONFIG_STR_MAX + 1u);
    fix_crc_v1();
    TEST_ASSERT_EQUAL(DFU_CFG_ERR_LEN,
        dfu_config_parse(s_blob, &mfr, &mfr_len, &prod, &prod_len));
}

static void test_len_exactly_max_is_allowed(void)
{
    const char *mfr, *prod;
    uint8_t mfr_len, prod_len;
    s_blob[DFU_CONFIG_OFF_MFR_LEN]  = (uint8_t)DFU_CONFIG_STR_MAX;
    s_blob[DFU_CONFIG_OFF_PROD_LEN] = (uint8_t)DFU_CONFIG_STR_MAX;
    fix_crc_v1();
    TEST_ASSERT_EQUAL(DFU_CFG_OK,
        dfu_config_parse(s_blob, &mfr, &mfr_len, &prod, &prod_len));
    TEST_ASSERT_EQUAL_UINT8(DFU_CONFIG_STR_MAX, mfr_len);
    TEST_ASSERT_EQUAL_UINT8(DFU_CONFIG_STR_MAX, prod_len);
}

static void test_bad_crc_fails(void)
{
    const char *mfr, *prod;
    uint8_t mfr_len, prod_len;
    s_blob[DFU_CONFIG_OFF_CRC_V1] ^= 0x01u;  /* flip a stored CRC bit */
    TEST_ASSERT_EQUAL(DFU_CFG_ERR_CRC,
        dfu_config_parse(s_blob, &mfr, &mfr_len, &prod, &prod_len));
}

static void test_payload_change_without_crc_fix_fails_crc(void)
{
    const char *mfr, *prod;
    uint8_t mfr_len, prod_len;
    s_blob[DFU_CONFIG_OFF_MFR] ^= 0x20u;  /* mutate a name byte, leave CRC */
    TEST_ASSERT_EQUAL(DFU_CFG_ERR_CRC,
        dfu_config_parse(s_blob, &mfr, &mfr_len, &prod, &prod_len));
}

static void test_empty_strings_are_valid(void)
{
    const char *mfr = NULL;
    const char *prod = NULL;
    uint8_t mfr_len = 0xAA;
    uint8_t prod_len = 0xAA;
    build_valid_v1_block("", 0u, "", 0u);
    TEST_ASSERT_EQUAL(DFU_CFG_OK,
        dfu_config_parse(s_blob, &mfr, &mfr_len, &prod, &prod_len));
    TEST_ASSERT_EQUAL_UINT8(0u, mfr_len);
    TEST_ASSERT_EQUAL_UINT8(0u, prod_len);
}

/* -----------------------------------------------------------------------------
 * v2 name parse
 * ----------------------------------------------------------------------------- */
static void test_v2_block_returns_strings(void)
{
    const char *mfr = NULL;
    const char *prod = NULL;
    uint8_t mfr_len = 0;
    uint8_t prod_len = 0;
    build_valid_v2_block(TEST_MFR, (uint8_t)strlen(TEST_MFR),
                         TEST_PROD, (uint8_t)strlen(TEST_PROD), 0x201u, 0x202u);

    TEST_ASSERT_EQUAL(DFU_CFG_OK,
        dfu_config_parse(s_blob, &mfr, &mfr_len, &prod, &prod_len));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)strlen(TEST_MFR), mfr_len);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)strlen(TEST_PROD), prod_len);
    TEST_ASSERT_EQUAL_MEMORY(TEST_MFR, mfr, mfr_len);
    TEST_ASSERT_EQUAL_MEMORY(TEST_PROD, prod, prod_len);
}

static void test_v2_bad_crc_fails(void)
{
    const char *mfr, *prod;
    uint8_t mfr_len, prod_len;
    build_valid_v2_block(TEST_MFR, (uint8_t)strlen(TEST_MFR),
                         TEST_PROD, (uint8_t)strlen(TEST_PROD), 0x201u, 0x202u);
    s_blob[DFU_CONFIG_OFF_CRC_V2] ^= 0x01u;  /* flip a stored v2 CRC bit */
    TEST_ASSERT_EQUAL(DFU_CFG_ERR_CRC,
        dfu_config_parse(s_blob, &mfr, &mfr_len, &prod, &prod_len));
}

/* v2 crc covers the ID fields: mutating an ID without re-crc must fail. */
static void test_v2_id_change_without_crc_fix_fails_crc(void)
{
    const char *mfr, *prod;
    uint8_t mfr_len, prod_len;
    build_valid_v2_block(TEST_MFR, (uint8_t)strlen(TEST_MFR),
                         TEST_PROD, (uint8_t)strlen(TEST_PROD), 0x201u, 0x202u);
    s_blob[DFU_CONFIG_OFF_BOOT_CMD_ID] ^= 0x01u;  /* mutate ID, leave CRC */
    TEST_ASSERT_EQUAL(DFU_CFG_ERR_CRC,
        dfu_config_parse(s_blob, &mfr, &mfr_len, &prod, &prod_len));
}

/* -----------------------------------------------------------------------------
 * CAN-ID extraction — the per-device feature and its fallbacks
 * ----------------------------------------------------------------------------- */
static void test_absent_block_yields_default_ids(void)
{
    uint16_t cmd_id = 0, reply_id = 0;
    memset(s_blob, 0xFF, sizeof(s_blob));  /* erased data flash */
    dfu_config_can_ids(s_blob, &cmd_id, &reply_id);
    TEST_ASSERT_EQUAL_HEX16(CAN_ID_HOST_TO_BOOT, cmd_id);
    TEST_ASSERT_EQUAL_HEX16(CAN_ID_BOOT_TO_HOST, reply_id);
}

static void test_v1_block_yields_default_ids(void)
{
    uint16_t cmd_id = 0, reply_id = 0;
    /* setUp already built a valid v1 block. */
    dfu_config_can_ids(s_blob, &cmd_id, &reply_id);
    TEST_ASSERT_EQUAL_HEX16(CAN_ID_HOST_TO_BOOT, cmd_id);
    TEST_ASSERT_EQUAL_HEX16(CAN_ID_BOOT_TO_HOST, reply_id);
}

static void test_v2_block_yields_provisioned_ids(void)
{
    uint16_t cmd_id = 0, reply_id = 0;
    build_valid_v2_block(TEST_MFR, (uint8_t)strlen(TEST_MFR),
                         TEST_PROD, (uint8_t)strlen(TEST_PROD), 0x101u, 0x102u);
    dfu_config_can_ids(s_blob, &cmd_id, &reply_id);
    TEST_ASSERT_EQUAL_HEX16(0x101u, cmd_id);
    TEST_ASSERT_EQUAL_HEX16(0x102u, reply_id);
}

static void test_v2_boundary_ids_accepted(void)
{
    uint16_t cmd_id = 0, reply_id = 0;
    build_valid_v2_block(TEST_MFR, (uint8_t)strlen(TEST_MFR),
                         TEST_PROD, (uint8_t)strlen(TEST_PROD),
                         0x000u, DFU_CONFIG_CAN_ID_MAX);
    dfu_config_can_ids(s_blob, &cmd_id, &reply_id);
    TEST_ASSERT_EQUAL_HEX16(0x000u, cmd_id);
    TEST_ASSERT_EQUAL_HEX16(DFU_CONFIG_CAN_ID_MAX, reply_id);
}

/* An out-of-range ID (0xFFFF = erased/unset, or > 0x7FF) falls back per-field. */
static void test_v2_out_of_range_ids_fall_back_individually(void)
{
    uint16_t cmd_id = 0, reply_id = 0;
    build_valid_v2_block(TEST_MFR, (uint8_t)strlen(TEST_MFR),
                         TEST_PROD, (uint8_t)strlen(TEST_PROD),
                         0xFFFFu, (uint16_t)(DFU_CONFIG_CAN_ID_MAX + 1u));
    dfu_config_can_ids(s_blob, &cmd_id, &reply_id);
    TEST_ASSERT_EQUAL_HEX16(CAN_ID_HOST_TO_BOOT, cmd_id);
    TEST_ASSERT_EQUAL_HEX16(CAN_ID_BOOT_TO_HOST, reply_id);
}

/* One valid, one out-of-range: only the bad field falls back. */
static void test_v2_mixed_ids_fall_back_only_bad_field(void)
{
    uint16_t cmd_id = 0, reply_id = 0;
    build_valid_v2_block(TEST_MFR, (uint8_t)strlen(TEST_MFR),
                         TEST_PROD, (uint8_t)strlen(TEST_PROD),
                         0x123u, 0xFFFFu);
    dfu_config_can_ids(s_blob, &cmd_id, &reply_id);
    TEST_ASSERT_EQUAL_HEX16(0x123u, cmd_id);
    TEST_ASSERT_EQUAL_HEX16(CAN_ID_BOOT_TO_HOST, reply_id);
}

/* A bad-crc v2 block must not leak its ID fields: defaults only. */
static void test_v2_bad_crc_yields_default_ids(void)
{
    uint16_t cmd_id = 0, reply_id = 0;
    build_valid_v2_block(TEST_MFR, (uint8_t)strlen(TEST_MFR),
                         TEST_PROD, (uint8_t)strlen(TEST_PROD), 0x101u, 0x102u);
    s_blob[DFU_CONFIG_OFF_CRC_V2] ^= 0x01u;
    dfu_config_can_ids(s_blob, &cmd_id, &reply_id);
    TEST_ASSERT_EQUAL_HEX16(CAN_ID_HOST_TO_BOOT, cmd_id);
    TEST_ASSERT_EQUAL_HEX16(CAN_ID_BOOT_TO_HOST, reply_id);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_valid_block_returns_strings);
    RUN_TEST(test_erased_block_fails_magic);
    RUN_TEST(test_bad_magic_fails);
    RUN_TEST(test_unknown_version_fails);
    RUN_TEST(test_mfr_len_over_max_fails);
    RUN_TEST(test_prod_len_over_max_fails);
    RUN_TEST(test_len_exactly_max_is_allowed);
    RUN_TEST(test_bad_crc_fails);
    RUN_TEST(test_payload_change_without_crc_fix_fails_crc);
    RUN_TEST(test_empty_strings_are_valid);
    RUN_TEST(test_v2_block_returns_strings);
    RUN_TEST(test_v2_bad_crc_fails);
    RUN_TEST(test_v2_id_change_without_crc_fix_fails_crc);
    RUN_TEST(test_absent_block_yields_default_ids);
    RUN_TEST(test_v1_block_yields_default_ids);
    RUN_TEST(test_v2_block_yields_provisioned_ids);
    RUN_TEST(test_v2_boundary_ids_accepted);
    RUN_TEST(test_v2_out_of_range_ids_fall_back_individually);
    RUN_TEST(test_v2_mixed_ids_fall_back_only_bad_field);
    RUN_TEST(test_v2_bad_crc_yields_default_ids);
    return UNITY_END();
}
