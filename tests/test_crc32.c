/**
 * @file    test_crc32.c
 * @brief   Host-side Unity tests for crc32_ieee.
 *
 * Golden vectors for the IEEE 802.3 / zlib-compatible CRC32 (reflected,
 * poly 0xEDB88320, init 0xFFFFFFFF, final XOR 0xFFFFFFFF).
 *
 * Covers:       The "123456789" check value, empty input, a single byte, an
 *               all-0xFF write unit, and crc32_ieee_update: zlib-style
 *               chaining over every split of a buffer equals the one-shot
 *               CRC, and an empty chunk leaves the running value unchanged.
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

static void test_update_over_any_split_matches_one_shot(void)
{
    /* The DFU path feeds the running CRC one USB block at a time; every split
     * point must give the same result as one pass over the whole buffer. */
    const uint8_t *msg = (const uint8_t *)"123456789";
    for (uint32_t split = 0u; split <= 9u; split++) {
        const uint32_t head = crc32_ieee_update(0u, msg, split);
        TEST_ASSERT_EQUAL_HEX32(0xCBF43926u,
                                crc32_ieee_update(head, &msg[split], 9u - split));
    }
}

static void test_update_with_empty_chunk_is_identity(void)
{
    const uint32_t running = crc32_ieee((const uint8_t *)"a", 1u);
    TEST_ASSERT_EQUAL_HEX32(running,
                            crc32_ieee_update(running, (const uint8_t *)"", 0u));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_check_value_123456789);
    RUN_TEST(test_empty_input_is_zero);
    RUN_TEST(test_single_byte_a);
    RUN_TEST(test_all_0xff_write_unit);
    RUN_TEST(test_update_over_any_split_matches_one_shot);
    RUN_TEST(test_update_with_empty_chunk_is_identity);
    return UNITY_END();
}
