/**
 * @file    cancfg.h
 * @brief   Partition identity shared by the early bootloader and CAN updater.
 *
 * Locates the provisioning data by table label/type rather than a fixed address,
 * so both supported layouts use the same readers. The CSVs own its location.
 *
 * @see     main/main.c, bootloader_components/main/boot.c, board_profile.h
 */
#ifndef UPDATER_CANCFG_H
#define UPDATER_CANCFG_H

#define CAN_CFG_PARTITION_LABEL   "cancfg"
#define CAN_CFG_PARTITION_SUBTYPE 0x40u

#endif
