/**
 * @file    image_validate.h
 * @brief   Pure-logic validation of the application image.
 *
 * Operates on a byte buffer standing in for code flash, so it compiles and unit-tests on the host
 * (tests/). Per rules/bootloader.md: never jump to an image that has not passed image_validate().
 *
 * Invariants:   Check order (fail closed, first failure wins): magic -> header CRC -> size
 *               bounds -> initial SP -> entry point -> payload CRC. The result codes follow that
 *               order and are a frozen contract.
 * Errors:       Returns IMAGE_VALID, or the image_validate_result_t of the first failed check.
 * Depends on:   image_header.h and memory_map.h; no flash or peripheral access.
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
    IMAGE_ERR_SP,           /* initial SP word outside SRAM                    */
    IMAGE_ERR_ENTRY,        /* entry outside the app slot or Thumb bit clear   */
    IMAGE_ERR_PAYLOAD_CRC,  /* image CRC32 mismatch                            */
} image_validate_result_t;

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

#ifdef __cplusplus
}
#endif

#endif /* IMAGE_VALIDATE_H */
