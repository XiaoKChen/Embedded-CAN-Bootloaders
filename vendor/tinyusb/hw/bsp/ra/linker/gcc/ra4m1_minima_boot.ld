/* UNO R4 Minima bootloader link (Makefile.minima's LD_FILE): ra4m1.ld with
 * the FLASH region bounded to the 16 KB bootloader region, so an image that
 * does not fit fails the link (fsp.ld limits FLASH to FLASH_IMAGE_LENGTH).
 * The other UNO R4 bootloaders (Makefile.wifi, Makefile.opta-*: BOSSA, with
 * a 32 KB region when combined with DFU) link ra4m1.ld directly and keep
 * the whole FLASH region.
 * The same 16 KB is written in three places; change them together: this
 * value, src/boot/memory_map.h BOOT_SIZE and Makefile.minima
 * BOOT_REGION_LIMIT (the .bin size gate). The ASSERT checks the result. */
FLASH_IMAGE_LENGTH = 0x4000;

INCLUDE ra4m1.ld

ASSERT(ORIGIN(FLASH) == 0 && LENGTH(FLASH) == 0x4000,
       "ra4m1_minima_boot.ld: FLASH must be the 16 KB bootloader region at 0 (memory_map.h BOOT_BASE/BOOT_SIZE)")
