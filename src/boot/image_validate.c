/**
 * @file    image_validate.c
 * @brief   Pure-logic validation of the application image (see
 *          image_validate.h for the contract, check order and boot decision).
 *
 * Implementation notes: fail-closed — every metadata field is bounds-checked
 * before it is trusted, and the expensive payload CRC runs last. All
 * multi-byte reads and writes go through explicit little-endian byte
 * accesses — no misaligned or aliased direct loads from the buffer.
 *
 * @see     image_validate.h, test_image_validate.c
 */

#include "image_validate.h"

#include <stdbool.h>

#include "crc32.h"

/* The stock Arduino bootloader's "is there a sketch" test, kept verbatim for
 * the no-record fallback: the SP word's top byte must be SRAM's (0x20). */
#define STOCK_SP_REGION_MASK 0xFF000000u

/* AAPCS: the stack is 8-byte aligned at every public interface, the reset
 * handler's entry included. */
#define INITIAL_SP_ALIGN 8u

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

static void store_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static bool pending_marker_present(const uint8_t *flash_base)
{
    const uint8_t *pending = flash_base + METADATA_ADDR + IMAGE_PENDING_OFFSET;
    return load_le32(&pending[0]) == IMAGE_PENDING_MAGIC &&
           load_le32(&pending[4]) == (uint32_t)~IMAGE_PENDING_MAGIC;
}

/* -----------------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------------- */
image_validate_result_t image_check_bounds(const uint8_t *app, uint32_t image_size)
{
    if (image_size < IMAGE_MIN_SIZE || image_size > APP_SLOT_SIZE) {
        return IMAGE_ERR_SIZE;
    }

    /* Initial SP (vector 0): must point into SRAM. SRAM_END itself is legal —
     * a full-descending stack top. SRAM_BASE itself would be a zero-size
     * stack, rejected. */
    const uint32_t initial_sp = load_le32(&app[0]);
    if (initial_sp <= SRAM_BASE || initial_sp > SRAM_END ||
        (initial_sp % INITIAL_SP_ALIGN) != 0u) {
        return IMAGE_ERR_SP;
    }

    /* Reset vector (vector 1): Thumb bit set, target inside the image's own
     * bytes — an entry past image_size would run unchecked flash. */
    const uint32_t entry = load_le32(&app[4]);
    if ((entry & 1u) == 0u) {
        return IMAGE_ERR_ENTRY;
    }
    const uint32_t entry_addr = entry & ~1u;
    if (entry_addr < APP_BASE || (entry_addr - APP_BASE) >= image_size) {
        return IMAGE_ERR_ENTRY;
    }
    return IMAGE_VALID;
}

image_validate_result_t image_validate(const uint8_t *flash_base)
{
    const uint8_t *meta = flash_base + METADATA_ADDR;

    if (load_le32(&meta[0]) != IMAGE_META_MAGIC) {
        return IMAGE_ERR_MAGIC;
    }
    if (crc32_ieee(meta, IMAGE_META_CRC_COVERAGE) != load_le32(&meta[12])) {
        return IMAGE_ERR_HEADER_CRC;
    }

    /* Fields are authenticated by the header CRC; now bounds-check before use. */
    const uint32_t image_size = load_le32(&meta[4]);
    const uint8_t *app = flash_base + APP_BASE;
    const image_validate_result_t bounds = image_check_bounds(app, image_size);
    if (bounds != IMAGE_VALID) {
        return bounds;
    }

    if (crc32_ieee(app, image_size) != load_le32(&meta[8])) {
        return IMAGE_ERR_PAYLOAD_CRC;
    }

    return IMAGE_VALID;
}

void image_meta_encode(uint8_t out[IMAGE_META_SIZE], uint32_t image_size,
                       uint32_t image_crc32)
{
    store_le32(&out[0], IMAGE_META_MAGIC);
    store_le32(&out[4], image_size);
    store_le32(&out[8], image_crc32);
    store_le32(&out[12], crc32_ieee(out, IMAGE_META_CRC_COVERAGE));
}

boot_decision_t boot_decide(const uint8_t *flash_base)
{
    const image_validate_result_t result = image_validate(flash_base);

    if (result == IMAGE_VALID) {
        return BOOT_DECISION_APP;
    }
    if (result != IMAGE_ERR_MAGIC && result != IMAGE_ERR_HEADER_CRC) {
        return BOOT_DECISION_PARK;
    }
    if (pending_marker_present(flash_base)) {
        return BOOT_DECISION_PARK;
    }
    if ((load_le32(flash_base + APP_BASE) & STOCK_SP_REGION_MASK) == SRAM_BASE) {
        return BOOT_DECISION_APP;
    }
    return BOOT_DECISION_PARK;
}
