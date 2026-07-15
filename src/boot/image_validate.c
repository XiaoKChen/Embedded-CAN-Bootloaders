/* =============================================================================
 * image_validate.c — Pure-logic validation of the application image
 * =============================================================================
 * See image_validate.h for the interface and check order. Fail-closed: every
 * metadata field is bounds-checked before it is trusted, and the expensive
 * payload CRC runs last. All multi-byte reads go through explicit little-
 * endian byte loads — no misaligned or aliased direct loads from the buffer.
 * ============================================================================= */

#include "image_validate.h"

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

/* -----------------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------------- */
image_validate_result_t image_validate(const uint8_t *flash_base)
{
    const uint8_t *meta = flash_base + METADATA_ADDR;

    if (load_le32(&meta[0]) != IMAGE_META_MAGIC) {
        return IMAGE_ERR_MAGIC;
    }
    if (crc32_ieee(meta, IMAGE_META_CRC_COVERAGE) != load_le32(&meta[12])) {
        return IMAGE_ERR_HEADER_CRC;
    }

    /* Fields are authenticated by the header CRC; now bounds-check before use.
     * Minimum size 8: the image must at least contain its SP and entry words. */
    const uint32_t image_size = load_le32(&meta[4]);
    if (image_size < 8u || image_size > APP_SLOT_SIZE) {
        return IMAGE_ERR_SIZE;
    }

    const uint8_t *app = flash_base + APP_BASE;

    /* Initial SP (vector 0): must point into SRAM. SRAM_END itself is legal —
     * a full-descending stack top. SRAM_BASE itself would be a zero-size
     * stack, rejected. */
    const uint32_t initial_sp = load_le32(&app[0]);
    if (initial_sp <= SRAM_BASE || initial_sp > SRAM_END) {
        return IMAGE_ERR_SP;
    }

    /* Reset vector (vector 1): Thumb bit set, target address inside the slot. */
    const uint32_t entry = load_le32(&app[4]);
    if ((entry & 1u) == 0u) {
        return IMAGE_ERR_ENTRY;
    }
    const uint32_t entry_addr = entry & ~1u;
    if (entry_addr < APP_BASE || entry_addr >= (APP_BASE + APP_SLOT_SIZE)) {
        return IMAGE_ERR_ENTRY;
    }

    if (crc32_ieee(app, image_size) != load_le32(&meta[8])) {
        return IMAGE_ERR_PAYLOAD_CRC;
    }

    return IMAGE_VALID;
}
