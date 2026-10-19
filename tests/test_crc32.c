/**
 * @file    test_crc32.c
 * @brief   Host-side Unity tests for crc32_ieee / crc32_ieee_update.
 *
 * Golden vectors for the IEEE 802.3 / zlib-compatible CRC32 (reflected, poly 0xEDB88320, init
 * 0xFFFFFFFF, final XOR 0xFFFFFFFF).
 *
 * Covers:       The "123456789" check value, empty input, a single byte, all-0xFF write units,
 *               and chunked updates matching the one-shot result.
 *
 * @see     crc32.h, rules/testing.md
 */

#include <string.h>

#include "unity.h"

#include "crc32.h"

void setUp(void) {}
void tearDown(void) {}

/* -----------------------------------------------------------------------------
 * Golden vectors
 * ----------------------------------------------------------------------------- */
static void test_check_value_123456789(void)
{
    /* The canonical CRC32 "check" value. */
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926u,
                            crc32_ieee((const uint8_t *)"123456789", 9u));
}

static void test_empty_input_is_zero(void)
{
    /* init ^ final-xor cancel: CRC of zero bytes is 0 (zlib crc32("") == 0). */
    TEST_ASSERT_EQUAL_HEX32(0x00000000u, crc32_ieee((const uint8_t *)"", 0u));
}

static void test_single_byte_a(void)
{
    TEST_ASSERT_EQUAL_HEX32(0xE8B7BE43u, crc32_ieee((const uint8_t *)"a", 1u));
}

static void test_all_0xff_write_unit(void)
{
    /* One erased flash write unit — exercises high-bit bytes. Cross-checked
     * against an independent zlib-compatible CRC32 implementation. */
    static const uint8_t erased[8] = {0xFFu, 0xFFu, 0xFFu, 0xFFu,
                                      0xFFu, 0xFFu, 0xFFu, 0xFFu};
    TEST_ASSERT_EQUAL_HEX32(0x2144DF1Cu, crc32_ieee(erased, sizeof(erased)));
}

static void test_update_over_chunks_matches_one_shot(void)
{
    /* zlib-style chaining: feeding "1234" then "56789" through
     * crc32_ieee_update equals the one-shot CRC of "123456789". */
    const uint8_t *msg = (const uint8_t *)"123456789";
    const uint32_t first = crc32_ieee_update(0u, msg, 4u);
    TEST_ASSERT_EQUAL_HEX32(crc32_ieee(msg, 4u), first);
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, crc32_ieee_update(first, &msg[4], 5u));
}

static void test_update_with_empty_chunk_is_identity(void)
{
    const uint32_t crc = crc32_ieee((const uint8_t *)"a", 1u);
    TEST_ASSERT_EQUAL_HEX32(crc, crc32_ieee_update(crc, (const uint8_t *)"", 0u));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_check_value_123456789);
    RUN_TEST(test_empty_input_is_zero);
    RUN_TEST(test_single_byte_a);
    RUN_TEST(test_all_0xff_write_unit);
    RUN_TEST(test_update_over_chunks_matches_one_shot);
    RUN_TEST(test_update_with_empty_chunk_is_identity);
    return UNITY_END();
}
