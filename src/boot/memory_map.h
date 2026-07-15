/* =============================================================================
 * memory_map.h — Flash/RAM partition for the UNO R4 Minima (RA4M1) bootloader
 * =============================================================================
 * Single header mirror of the flash partition. The linker script (vendored
 * bootloader layout, see minima_fsp.ld / the arduino-renesas-bootloader fork)
 * is the source of truth; these constants MUST agree with it. Do not scatter
 * these addresses anywhere else.
 *
 * Partition (256 KB code flash, 128 x 2 KB erase blocks):
 *   0x0000_0000 .. 0x0000_3FFF   bootloader (8 blocks, immutable at runtime,
 *                                incl. OFS0/OFS1 option bytes at 0x400/0x404)
 *   0x0000_4000 .. 0x0003_F7FF   application slot (119 blocks)
 *   0x0003_F800 .. 0x0003_FFFF   image metadata block (last block, written
 *                                LAST during an update — see image_header.h)
 *
 * VTOR policy: the BOOTLOADER sets SCB->VTOR = APP_BASE immediately before
 * the jump (stock boot5() behavior); the application does not need to.
 *
 * Pure host-compilable header: <stdint.h> only, no target includes.
 * ============================================================================= */

#ifndef MEMORY_MAP_H
#define MEMORY_MAP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -----------------------------------------------------------------------------
 * Code flash partition
 * ----------------------------------------------------------------------------- */
#define FLASH_BASE          0x00000000u
#define FLASH_SIZE          0x00040000u  /* 256 KB total code flash            */
#define FLASH_BLOCK_SIZE    2048u        /* code flash erase block             */
#define FLASH_WRITE_UNIT    8u           /* code flash write unit (r_flash_lp) */

#define BOOT_BASE           FLASH_BASE
#define BOOT_SIZE           0x00004000u  /* 16 KB, 8 blocks — never self-written */

#define APP_BASE            0x00004000u  /* app vector table lives here        */
#define APP_SLOT_SIZE       0x0003B800u  /* 238 KB, 119 blocks                 */

#define METADATA_ADDR       0x0003F800u  /* last 2 KB block                    */
#define METADATA_BLOCK_SIZE FLASH_BLOCK_SIZE

/* -----------------------------------------------------------------------------
 * Data flash (RA4M1: 8 KB at 0x4010_0000..0x4010_1FFF, 1 KB erase blocks).
 * Separately mapped from code flash; readable via normal loads once
 * R_FLASH_LP_Open() has enabled DFLCTL.DFLEN. Holds the optional flash-time DFU
 * name config block (dfu_config.h) in the last 1 KB block — frozen shared
 * contract with the provisioning flasher, do not move.
 * ----------------------------------------------------------------------------- */
#define DATA_FLASH_BASE        0x40100000u
#define DATA_FLASH_SIZE        0x00002000u  /* 8 KB                              */
#define DATA_FLASH_BLOCK_SIZE  1024u        /* data flash erase block            */

#define DFU_CONFIG_ADDR        0x40101C00u  /* last 1 KB block; DFU name config  */

/* -----------------------------------------------------------------------------
 * SRAM
 * ----------------------------------------------------------------------------- */
#define SRAM_BASE           0x20000000u
#define SRAM_SIZE           0x00008000u  /* 32 KB */
#define SRAM_END            (SRAM_BASE + SRAM_SIZE)  /* 0x20008000, exclusive/
                                                        top-of-stack limit    */

/* -----------------------------------------------------------------------------
 * Partition consistency (compile-time; C11)
 * ----------------------------------------------------------------------------- */
_Static_assert(BOOT_BASE + BOOT_SIZE == APP_BASE,
               "app slot must start where the bootloader region ends");
_Static_assert(APP_BASE + APP_SLOT_SIZE == METADATA_ADDR,
               "metadata block must start where the app slot ends");
_Static_assert(METADATA_ADDR + METADATA_BLOCK_SIZE == FLASH_BASE + FLASH_SIZE,
               "metadata block must be the last flash block");
_Static_assert(APP_SLOT_SIZE % FLASH_BLOCK_SIZE == 0u,
               "app slot must be a whole number of erase blocks");
_Static_assert(DFU_CONFIG_ADDR + DATA_FLASH_BLOCK_SIZE ==
               DATA_FLASH_BASE + DATA_FLASH_SIZE,
               "DFU config block must be the last data-flash block");

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_MAP_H */
