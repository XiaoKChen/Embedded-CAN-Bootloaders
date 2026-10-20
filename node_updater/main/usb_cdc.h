/**
 * @file    usb_cdc.h
 * @brief   Provisioned USB CDC identity and 1200-baud ROM-download entry.
 *
 * Exposes the updater as a CDC-ACM device using the frozen v2 configuration's
 * names and the chip MAC. A 1200-baud DTR falling edge restarts into ROM download.
 *
 * Owns:         USB descriptors and CDC line-state callbacks.
 * Not here:     UART0 logging and CAN firmware transfer (main.c).
 * Invariants:   Descriptor storage lives for the lifetime of the USB driver.
 * Concurrency:  Call init once from app_main; callbacks run in TinyUSB's task.
 * Errors:       Initialization returns an ESP-IDF error; CAN may still run.
 *
 * @see     usb_cdc.c, dfu_config.h
 */
#ifndef UPDATER_USB_CDC_H
#define UPDATER_USB_CDC_H

#include <stdint.h>
#include "esp_err.h"
#include "dfu_config.h"

esp_err_t updater_usb_cdc_init(const uint8_t config[DFU_CONFIG_SIZE]);

#endif
