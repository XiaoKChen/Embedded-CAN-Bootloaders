/* =============================================================================
 * image_validate.c — Pure-logic validation of the application image
 * =============================================================================
 * See image_validate.h for the interface, check order and boot decision.
 * Fail-closed: every metadata field is bounds-checked before it is trusted,
 * and the expensive payload CRC runs last. All multi-byte reads go through
 * explicit little-endian byte loads — no misaligned or aliased direct loads
 * from the buffer.
 * ============================================================================= */

#include "image_validate.h"

#include "crc32.h"

/* The stock Arduino bootloader's "is there a sketch" test, kept verbatim for
 * the no-record fallback: the SP word's top byte must be SRAM's (0x20). */
#define STOCK_SP_REGION_MASK 0xFF000000u

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

boot_decision_t boot_decide(const uint8_t *flash_base)
{
    const image_validate_result_t result = image_validate(flash_base);

    if (result == IMAGE_VALID) {
        return BOOT_DECISION_APP;
    }
    if (result != IMAGE_ERR_MAGIC && result != IMAGE_ERR_HEADER_CRC) {
        return BOOT_DECISION_PARK;
    }

    const uint8_t *pending = flash_base + METADATA_ADDR + IMAGE_PENDING_OFFSET;
    if (load_le32(&pending[0]) == IMAGE_PENDING_MAGIC &&
        load_le32(&pending[4]) == (uint32_t)~IMAGE_PENDING_MAGIC) {
        return BOOT_DECISION_PARK;
    }

    if ((load_le32(flash_base + APP_BASE) & STOCK_SP_REGION_MASK) == SRAM_BASE) {
        return BOOT_DECISION_APP;
    }
    return BOOT_DECISION_PARK;
}
