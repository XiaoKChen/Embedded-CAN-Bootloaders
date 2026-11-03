RAM_START = 0x20000000;
RAM_LENGTH = 0x8000;
FLASH_START = 0x00000000;
FLASH_LENGTH = 0x40000;
/* UNO R4 bootloader (Makefile.minima links this file): fsp.ld limits the
 * FLASH region to FLASH_IMAGE_LENGTH, so an image larger than the 16 KB
 * bootloader region (src/boot/memory_map.h BOOT_SIZE) fails the link.
 * The same 16 KB is written in three places; change them together:
 * this value, memory_map.h BOOT_SIZE and Makefile.minima BOOT_REGION_LIMIT
 * (the .bin size gate). The ASSERT at the end checks the result. */
FLASH_IMAGE_LENGTH = 0x4000;
DATA_FLASH_START = 0x40100000;
DATA_FLASH_LENGTH = 0x2000;
OPTION_SETTING_START = 0x00000000;
OPTION_SETTING_LENGTH = 0x0;
OPTION_SETTING_S_START = 0x80000000;
OPTION_SETTING_S_LENGTH = 0x0;
ID_CODE_START = 0x01010018;
ID_CODE_LENGTH = 0x20;
SDRAM_START = 0x80010000;
SDRAM_LENGTH = 0x0;
QSPI_FLASH_START = 0x60000000;
QSPI_FLASH_LENGTH = 0x0;
OSPI_DEVICE_0_START = 0x80020000;
OSPI_DEVICE_0_LENGTH = 0x0;
OSPI_DEVICE_1_START = 0x80030000;
OSPI_DEVICE_1_LENGTH = 0x0;

INCLUDE fsp.ld

ASSERT(ORIGIN(FLASH) == 0 && LENGTH(FLASH) == 0x4000,
       "ra4m1.ld: FLASH must be the 16 KB bootloader region at 0 (memory_map.h BOOT_BASE/BOOT_SIZE)")
