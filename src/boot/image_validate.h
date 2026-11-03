/**
 * @file    image_validate.h
 * @brief   Pure-logic validation of the application image, the boot
 *          decision, and the record and erase plan of an update.
 *
 * Operates on a byte buffer standing in for code flash, so it compiles and
 * unit-tests on the host (tests/). Per rules/bootloader.md: never jump to an
 * image that has not passed image_validate().
 *
 * boot_decide() turns the validation result plus the update-in-progress
 * marker (image_header.h) into the bootloader's app-or-park decision:
 *   record valid ........................... APP (marker irrelevant)
 *   record present, any later check fails .. PARK
 *   no record (magic / header CRC) + marker  PARK (interrupted update)
 *   no record, no marker ................... stock SP-word fallback, for
 *                                            images flashed without a record
 *                                            AND with the metadata block
 *                                            erased (e.g. SWD after a full
 *                                            chip erase): APP iff the SP word
 *                                            at APP_BASE is 0x20xx_xxxx
 * An SWD flash over a board that once took a DFU/CAN update must also erase
 * the metadata block (0x3F800): the stale record no longer matches the new
 * image and parks the board.
 *
 * update_start_plan() decides what an update must erase before it touches
 * the app slot, so that no prefix of those erases can take the SP fallback
 * into a partial image (retried and interrupted updates included).
 *
 * Owns:         The image validation checks, the app-or-park decision, the
 *               metadata record encoding and the update-start plan.
 * Not here:     The record and marker layout (image_header.h); the jump to
 *               the application (boot5() in the fork's src/main.c); the
 *               flash writes themselves (the callers' HAL).
 * Invariants:   Check order (fail closed, first failure wins): magic ->
 *               header CRC -> size bounds -> initial SP -> entry point ->
 *               payload CRC. The result codes follow that order and are a
 *               frozen contract. The initial SP is 8-byte aligned and inside
 *               (SRAM_BASE, SRAM_END]; the entry point lies inside the
 *               image's own bytes.
 * Errors:       image_validate() returns IMAGE_VALID, or the
 *               image_validate_result_t of the first failed check.
 * Depends on:   image_header.h and memory_map.h; no flash or peripheral
 *               access.
 *
 * @see     image_validate.c, test_image_validate.c, rules/bootloader.md
 */

#ifndef IMAGE_VALIDATE_H
#define IMAGE_VALIDATE_H

#include <stdint.h>

#include "image_header.h"
#include "memory_map.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -----------------------------------------------------------------------------
 * Result codes — ordered to match the check order; frozen contract
 * ----------------------------------------------------------------------------- */
typedef enum {
    IMAGE_VALID = 0,
    IMAGE_ERR_MAGIC,        /* metadata magic missing (e.g. erased block)      */
    IMAGE_ERR_HEADER_CRC,   /* metadata self-CRC mismatch                      */
    IMAGE_ERR_SIZE,         /* image_size outside [8, APP_SLOT_SIZE]           */
    IMAGE_ERR_SP,           /* initial SP outside SRAM or not 8-byte aligned   */
    IMAGE_ERR_ENTRY,        /* entry outside the image or Thumb bit clear      */
    IMAGE_ERR_PAYLOAD_CRC,  /* image CRC32 mismatch                            */
} image_validate_result_t;

typedef enum {
    BOOT_DECISION_PARK = 0,  /* stay in the bootloader (DFU + CAN)            */
    BOOT_DECISION_APP,       /* jump to the image at APP_BASE                 */
} boot_decision_t;

/* What an update must do to the metadata block (and the app slot's first
 * block) before it erases the rest of the app slot. */
typedef enum {
    UPDATE_START_MARKER_IN_PLACE = 0,     /* marker set, record area blank:
                                             erase nothing, keep the marker   */
    UPDATE_START_ERASE_METADATA,          /* slot bootable: erase the block,
                                             program the marker               */
    UPDATE_START_ERASE_APP_THEN_METADATA, /* slot not bootable: erase the
                                             APP_BASE block first, so the SP
                                             fallback finds no SP word        */
} update_start_t;

/* -----------------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------------- */

/**
 * Validate the application image described by the metadata block.
 *
 * @param flash_base  Base of code flash as a byte buffer: metadata is read at
 *                    flash_base + METADATA_ADDR, the app at flash_base +
 *                    APP_BASE. On target this is (const uint8_t *)FLASH_BASE;
 *                    in host tests, a 256 KB array.
 * @return IMAGE_VALID when every check passes; the first failure otherwise.
 */
image_validate_result_t image_validate(const uint8_t *flash_base);

/**
 * The size, initial-SP and entry-point checks of image_validate(), for an
 * image whose record is not written yet (the update paths run it before
 * they commit).
 *
 * @param app         The image's first byte (APP_BASE on target).
 * @param image_size  Bytes of the image.
 * @return IMAGE_VALID, IMAGE_ERR_SIZE, IMAGE_ERR_SP or IMAGE_ERR_ENTRY.
 */
image_validate_result_t image_check_bounds(const uint8_t *app, uint32_t image_size);

/**
 * Encode the metadata record (magic, size, CRC, header CRC) in its flash
 * byte order. The one encoder both update paths program.
 */
void image_meta_encode(uint8_t out[IMAGE_META_SIZE], uint32_t image_size,
                       uint32_t image_crc32);

/**
 * Decide whether to boot the application or park in the bootloader (rules in
 * the file header above).
 *
 * @param flash_base  Same buffer contract as image_validate().
 * @return BOOT_DECISION_APP or BOOT_DECISION_PARK.
 */
boot_decision_t boot_decide(const uint8_t *flash_base);

/**
 * Decide what a starting update erases before the marker is in place.
 *
 * Marker present and record area blank (an earlier update was interrupted)
 * -> keep both: erasing would drop the marker and let the SP fallback boot
 * the partial image. Otherwise the metadata block is erased; when the slot
 * is not bootable now (boot_decide() == PARK) the APP_BASE block goes first.
 *
 * @param flash_base  Same buffer contract as image_validate().
 */
update_start_t update_start_plan(const uint8_t *flash_base);

#ifdef __cplusplus
}
#endif

#endif /* IMAGE_VALIDATE_H */
