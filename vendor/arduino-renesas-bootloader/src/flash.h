#if BSP_FEATURE_FLASH_LP_VERSION
   #include "r_flash_lp.h"
   #define flash_instance_ctrl_t flash_lp_instance_ctrl_t
   #define flash_apis g_flash_on_flash_lp
   #define PROGRAM_BLOCK_SIZE                (2 * 1024)
   #if defined BOSSA_LOADER && defined DFU_LOADER
      #define SKETCH_FLASH_OFFSET               (32 * 1024)
   #else
      #define SKETCH_FLASH_OFFSET               (16 * 1024)
   #endif
#endif

#if BSP_FEATURE_FLASH_HP_VERSION
   #include "r_flash_hp.h"
   #define flash_instance_ctrl_t flash_hp_instance_ctrl_t
   #define flash_apis g_flash_on_flash_hp
   #define PROGRAM_BLOCK_SIZE                (32 * 1024)
   #define SKETCH_FLASH_OFFSET               (64 * 1024)
   #define ZEPHYR_SKETCH_FLASH_OFFSET        (1024 * 1024)
   #define TURN_OFF_CHARGER_LED
   void i2c_begin();
   void i2c_write(uint8_t address, uint8_t reg, uint8_t value);
#endif

#ifdef CAN_LOADER
#include <stdbool.h>
/* Data-flash read gate (main.c flash_lp_open_df_read_ready): true only while
 * DFU_CONFIG_ADDR is provably readable; cleared by a failed flash erase or
 * program. Nothing reads DFU_CONFIG_ADDR while it is false (main.c INFO and
 * CAN IDs, usb_descriptors.c name strings). */
extern bool dfu_config_readable;
#endif

/* Key code for writing PRCR register. */
#define BSP_PRV_PRCR_KEY	 (0xA500U)
#define BSP_PRV_PRCR_PRC1_UNLOCK ((BSP_PRV_PRCR_KEY) | 0x2U)
#define BSP_PRV_PRCR_LOCK	 ((BSP_PRV_PRCR_KEY) | 0x0U)