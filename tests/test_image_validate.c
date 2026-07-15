/* =============================================================================
 * test_image_validate.c — Host-side Unity tests for image_validate
 * =============================================================================
 * Adapted from .claude/template/bootloader/test_image_validate.c to the
 * metadata-block layout: a 256 KB byte buffer stands in for code flash, the
 * app image sits at APP_BASE and the 16-byte metadata record at METADATA_ADDR.
 * Every image_validate_result_t outcome gets a test (rules/testing.md).
 * ============================================================================= */

#include <string.h>

#include "unity.h"

#include "crc32.h"
#include "image_validate.h"

/* -----------------------------------------------------------------------------
 * Fixture: a minimal valid image built in a fake 256 KB flash
 * ----------------------------------------------------------------------------- */
#define TEST_IMAGE_SIZE 64u
#define TEST_INITIAL_SP SRAM_END                /* full-descending stack top   */
#define TEST_ENTRY      ((APP_BASE + 0x100u) | 1u)  /* Thumb bit set           */

static uint8_t s_flash[FLASH_SIZE];

static void store_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

/* Write a metadata record with a self-consistent header CRC. */
static void write_metadata(uint32_t image_size, uint32_t image_crc)
{
    uint8_t *meta = &s_flash[METADATA_ADDR];
    store_le32(&meta[0], IMAGE_META_MAGIC);
    store_le32(&meta[4], image_size);
    store_le32(&meta[8], image_crc);
    store_le32(&meta[12], crc32_ieee(meta, IMAGE_META_CRC_COVERAGE));
}

static void build_valid_image(void)
{
    memset(s_flash, 0xFF, sizeof(s_flash));

    uint8_t *app = &s_flash[APP_BASE];
    for (uint32_t i = 0u; i < TEST_IMAGE_SIZE; i++) {
        app[i] = (uint8_t)(i * 7u);  /* arbitrary deterministic payload */
    }
    store_le32(&app[0], TEST_INITIAL_SP);  /* vector 0: initial SP  */
    store_le32(&app[4], TEST_ENTRY);       /* vector 1: reset entry */

    write_metadata(TEST_IMAGE_SIZE, crc32_ieee(app, TEST_IMAGE_SIZE));
}

/* Patch the app's SP/entry words; the payload CRC check runs AFTER the SP and
 * entry checks, so the stale image_crc32 never masks the expected error. */
static void patch_app_word(uint32_t offset, uint32_t value)
{
    store_le32(&s_flash[APP_BASE + offset], value);
}

void setUp(void)    { build_valid_image(); }
void tearDown(void) {}

/* -----------------------------------------------------------------------------
 * Tests — one per image_validate_result_t outcome, plus boundary cases
 * ----------------------------------------------------------------------------- */
static void test_valid_image_passes(void)
{
    TEST_ASSERT_EQUAL(IMAGE_VALID, image_validate(s_flash));
}

static void test_erased_metadata_fails_magic(void)
{
    memset(&s_flash[METADATA_ADDR], 0xFF, IMAGE_META_SIZE);
    TEST_ASSERT_EQUAL(IMAGE_ERR_MAGIC, image_validate(s_flash));
}

static void test_bad_magic_fails(void)
{
    s_flash[METADATA_ADDR] ^= 0x01u;
    TEST_ASSERT_EQUAL(IMAGE_ERR_MAGIC, image_validate(s_flash));
}

static void test_corrupt_metadata_field_fails_header_crc(void)
{
    s_flash[METADATA_ADDR + 4u] ^= 0x01u;  /* image_size LSB, CRC not fixed */
    TEST_ASSERT_EQUAL(IMAGE_ERR_HEADER_CRC, image_validate(s_flash));
}

static void test_size_zero_fails(void)
{
    write_metadata(0u, 0u);
    TEST_ASSERT_EQUAL(IMAGE_ERR_SIZE, image_validate(s_flash));
}

static void test_size_below_vector_minimum_fails(void)
{
    write_metadata(7u, 0u);  /* < 8: cannot even hold SP + entry */
    TEST_ASSERT_EQUAL(IMAGE_ERR_SIZE, image_validate(s_flash));
}

static void test_size_above_slot_fails(void)
{
    write_metadata(APP_SLOT_SIZE + 1u, 0u);
    TEST_ASSERT_EQUAL(IMAGE_ERR_SIZE, image_validate(s_flash));
}

static void test_size_exactly_slot_is_allowed(void)
{
    /* Boundary: image_size == APP_SLOT_SIZE passes the size check (payload
     * CRC then decides — here it matches, so the whole image validates). */
    write_metadata(APP_SLOT_SIZE, crc32_ieee(&s_flash[APP_BASE], APP_SLOT_SIZE));
    TEST_ASSERT_EQUAL(IMAGE_VALID, image_validate(s_flash));
}

static void test_sp_below_sram_fails(void)
{
    patch_app_word(0u, 0x10000000u);
    TEST_ASSERT_EQUAL(IMAGE_ERR_SP, image_validate(s_flash));
}

static void test_sp_at_sram_base_fails(void)
{
    patch_app_word(0u, SRAM_BASE);  /* zero-size stack: nonsense, reject */
    TEST_ASSERT_EQUAL(IMAGE_ERR_SP, image_validate(s_flash));
}

static void test_sp_above_sram_end_fails(void)
{
    patch_app_word(0u, SRAM_END + 4u);
    TEST_ASSERT_EQUAL(IMAGE_ERR_SP, image_validate(s_flash));
}

static void test_entry_in_bootloader_fails(void)
{
    patch_app_word(4u, BOOT_BASE | 1u);
    TEST_ASSERT_EQUAL(IMAGE_ERR_ENTRY, image_validate(s_flash));
}

static void test_entry_in_metadata_block_fails(void)
{
    patch_app_word(4u, METADATA_ADDR | 1u);  /* one past the slot end */
    TEST_ASSERT_EQUAL(IMAGE_ERR_ENTRY, image_validate(s_flash));
}

static void test_entry_thumb_bit_clear_fails(void)
{
    patch_app_word(4u, APP_BASE + 0x100u);  /* in range but ARM-state address */
    TEST_ASSERT_EQUAL(IMAGE_ERR_ENTRY, image_validate(s_flash));
}

static void test_corrupt_payload_fails_payload_crc(void)
{
    s_flash[APP_BASE + 9u] ^= 0x40u;  /* single bit flip past the vectors */
    TEST_ASSERT_EQUAL(IMAGE_ERR_PAYLOAD_CRC, image_validate(s_flash));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_valid_image_passes);
    RUN_TEST(test_erased_metadata_fails_magic);
    RUN_TEST(test_bad_magic_fails);
    RUN_TEST(test_corrupt_metadata_field_fails_header_crc);
    RUN_TEST(test_size_zero_fails);
    RUN_TEST(test_size_below_vector_minimum_fails);
    RUN_TEST(test_size_above_slot_fails);
    RUN_TEST(test_size_exactly_slot_is_allowed);
    RUN_TEST(test_sp_below_sram_fails);
    RUN_TEST(test_sp_at_sram_base_fails);
    RUN_TEST(test_sp_above_sram_end_fails);
    RUN_TEST(test_entry_in_bootloader_fails);
    RUN_TEST(test_entry_in_metadata_block_fails);
    RUN_TEST(test_entry_thumb_bit_clear_fails);
    RUN_TEST(test_corrupt_payload_fails_payload_crc);
    return UNITY_END();
}
