/* =============================================================================
 * test_dfu_config.c — Host-side Unity tests for dfu_config_parse
 * =============================================================================
 * A 76-byte buffer stands in for the RA4M1 data-flash config block. Every
 * dfu_config_status_t outcome gets a test (rules/testing.md), plus the empty-
 * string boundary. Pure logic: no hardware, mirrors test_image_validate.c.
 * ============================================================================= */

#include <string.h>

#include "unity.h"

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

/* Recompute and store the crc32 over the first 72 bytes. */
static void fix_crc(void)
{
    store_le32(&s_blob[DFU_CONFIG_OFF_CRC],
               crc32_ieee(s_blob, DFU_CONFIG_CRC_COVERAGE));
}

static void build_valid_block(const char *mfr, uint8_t mfr_len,
                              const char *prod, uint8_t prod_len)
{
    memset(s_blob, 0xFF, sizeof(s_blob));  /* erased-flash fill; pads = 0xFF */
    store_le32(&s_blob[DFU_CONFIG_OFF_MAGIC], DFU_CONFIG_MAGIC);
    s_blob[DFU_CONFIG_OFF_VERSION]  = (uint8_t)DFU_CONFIG_VERSION;
    s_blob[DFU_CONFIG_OFF_MFR_LEN]  = mfr_len;
    s_blob[DFU_CONFIG_OFF_PROD_LEN] = prod_len;
    s_blob[DFU_CONFIG_OFF_RESERVED] = 0xFFu;
    memcpy(&s_blob[DFU_CONFIG_OFF_MFR], mfr, mfr_len);
    memcpy(&s_blob[DFU_CONFIG_OFF_PROD], prod, prod_len);
    fix_crc();
}

void setUp(void)
{
    build_valid_block(TEST_MFR, (uint8_t)strlen(TEST_MFR),
                      TEST_PROD, (uint8_t)strlen(TEST_PROD));
}
void tearDown(void) {}

/* -----------------------------------------------------------------------------
 * Tests — one per dfu_config_status_t outcome, plus boundaries
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

static void test_bad_version_fails(void)
{
    const char *mfr, *prod;
    uint8_t mfr_len, prod_len;
    s_blob[DFU_CONFIG_OFF_VERSION] = 2u;
    fix_crc();  /* version is checked before CRC; keep CRC honest anyway */
    TEST_ASSERT_EQUAL(DFU_CFG_ERR_VERSION,
        dfu_config_parse(s_blob, &mfr, &mfr_len, &prod, &prod_len));
}

static void test_mfr_len_over_max_fails(void)
{
    const char *mfr, *prod;
    uint8_t mfr_len, prod_len;
    s_blob[DFU_CONFIG_OFF_MFR_LEN] = (uint8_t)(DFU_CONFIG_STR_MAX + 1u);
    fix_crc();
    TEST_ASSERT_EQUAL(DFU_CFG_ERR_LEN,
        dfu_config_parse(s_blob, &mfr, &mfr_len, &prod, &prod_len));
}

static void test_prod_len_over_max_fails(void)
{
    const char *mfr, *prod;
    uint8_t mfr_len, prod_len;
    s_blob[DFU_CONFIG_OFF_PROD_LEN] = (uint8_t)(DFU_CONFIG_STR_MAX + 1u);
    fix_crc();
    TEST_ASSERT_EQUAL(DFU_CFG_ERR_LEN,
        dfu_config_parse(s_blob, &mfr, &mfr_len, &prod, &prod_len));
}

static void test_len_exactly_max_is_allowed(void)
{
    const char *mfr, *prod;
    uint8_t mfr_len, prod_len;
    s_blob[DFU_CONFIG_OFF_MFR_LEN]  = (uint8_t)DFU_CONFIG_STR_MAX;
    s_blob[DFU_CONFIG_OFF_PROD_LEN] = (uint8_t)DFU_CONFIG_STR_MAX;
    fix_crc();
    TEST_ASSERT_EQUAL(DFU_CFG_OK,
        dfu_config_parse(s_blob, &mfr, &mfr_len, &prod, &prod_len));
    TEST_ASSERT_EQUAL_UINT8(DFU_CONFIG_STR_MAX, mfr_len);
    TEST_ASSERT_EQUAL_UINT8(DFU_CONFIG_STR_MAX, prod_len);
}

static void test_bad_crc_fails(void)
{
    const char *mfr, *prod;
    uint8_t mfr_len, prod_len;
    s_blob[DFU_CONFIG_OFF_CRC] ^= 0x01u;  /* flip a stored CRC bit */
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
    build_valid_block("", 0u, "", 0u);
    TEST_ASSERT_EQUAL(DFU_CFG_OK,
        dfu_config_parse(s_blob, &mfr, &mfr_len, &prod, &prod_len));
    TEST_ASSERT_EQUAL_UINT8(0u, mfr_len);
    TEST_ASSERT_EQUAL_UINT8(0u, prod_len);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_valid_block_returns_strings);
    RUN_TEST(test_erased_block_fails_magic);
    RUN_TEST(test_bad_magic_fails);
    RUN_TEST(test_bad_version_fails);
    RUN_TEST(test_mfr_len_over_max_fails);
    RUN_TEST(test_prod_len_over_max_fails);
    RUN_TEST(test_len_exactly_max_is_allowed);
    RUN_TEST(test_bad_crc_fails);
    RUN_TEST(test_payload_change_without_crc_fix_fails_crc);
    RUN_TEST(test_empty_strings_are_valid);
    return UNITY_END();
}
