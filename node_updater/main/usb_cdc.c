/**
 * @file    usb_cdc.c
 * @brief   Provisioned USB CDC and ROM-download touch (see usb_cdc.h).
 *
 * Implementation notes: only TinyUSB's task changes the touch state. USB
 * strings are copied out of the length-delimited v2 block into static buffers.
 * UART0 remains the console: CDC never blocks CAN service or log producers.
 *
 * @see     usb_cdc.h
 */
#include "usb_cdc.h"

#include <stdio.h>
#include <string.h>
#include "esp_mac.h"
#include "esp_system.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/soc.h"
#include "tinyusb.h"
#include "tusb_cdc_acm.h"

#define USB_VID                 0x303Au
#define USB_PID                 0x1001u
#define USB_TOUCH_BAUD          1200u
#define USB_SERIAL_LENGTH       12u
#define USB_CONFIG_LENGTH       (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN)
#define USB_CDC_NOTIFICATION_EP 0x81u
#define USB_CDC_OUT_EP          0x02u
#define USB_CDC_IN_EP           0x82u
#define USB_PACKET_SIZE         64u
#define USB_NOTIFICATION_SIZE   8u
#define USB_MAX_POWER_MA         100u

static char s_manufacturer[DFU_CONFIG_STR_MAX + 1u] = "Normal Corporation";
static char s_product[DFU_CONFIG_STR_MAX + 1u] = "CAN Updater";
static char s_serial[USB_SERIAL_LENGTH + 1u];
static const char *s_strings[] = {
    "\x09\x04", s_manufacturer, s_product, s_serial, "CAN Updater CDC",
};
static bool s_touch_armed;
static bool s_dtr;

static const tusb_desc_device_t s_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USB_VID,
    .idProduct = USB_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};
static const uint8_t s_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, USB_CONFIG_LENGTH, 0, USB_MAX_POWER_MA),
    TUD_CDC_DESCRIPTOR(0, 4, USB_CDC_NOTIFICATION_EP, USB_NOTIFICATION_SIZE,
                       USB_CDC_OUT_EP, USB_CDC_IN_EP, USB_PACKET_SIZE),
};

static void line_coding_changed(int itf, cdcacm_event_t *event)
{
    (void)itf;
    s_touch_armed = event->line_coding_changed_data.p_line_coding->bit_rate == USB_TOUCH_BAUD;
}

static void line_state_changed(int itf, cdcacm_event_t *event)
{
    (void)itf;
    const bool dtr = event->line_state_changed_data.dtr;
    const bool download = s_touch_armed && s_dtr && !dtr;
    s_dtr = dtr;
    if (download) {
        /* ESP32-S3 TRM RTC_CNTL_OPTION1: software strap into the ROM loader.
         * No flash/otadata write: interruption leaves the updater bootable. */
        REG_SET_BIT(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
        esp_restart();
    }
}

/* Do not let stale touch state survive a USB unplug/re-enumeration. */
void tud_umount_cb(void)
{
    s_touch_armed = false;
    s_dtr = false;
}

esp_err_t updater_usb_cdc_init(const uint8_t config[DFU_CONFIG_SIZE])
{
    const char *manufacturer;
    const char *product;
    uint8_t manufacturer_len;
    uint8_t product_len;
    if (dfu_config_parse(config, &manufacturer, &manufacturer_len,
                         &product, &product_len) == DFU_CFG_OK) {
        memcpy(s_manufacturer, manufacturer, manufacturer_len);
        s_manufacturer[manufacturer_len] = '\0';
        memcpy(s_product, product, product_len);
        s_product[product_len] = '\0';
    }
    uint8_t mac[6];
    esp_err_t err = esp_efuse_mac_get_default(mac);
    if (err != ESP_OK) {
        return err;
    }
    snprintf(s_serial, sizeof(s_serial), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    const tinyusb_config_t usb = {
        .device_descriptor = &s_device,
        .string_descriptor = s_strings,
        .string_descriptor_count = sizeof(s_strings) / sizeof(s_strings[0]),
        .configuration_descriptor = s_configuration,
    };
    err = tinyusb_driver_install(&usb);
    if (err != ESP_OK) {
        return err;
    }
    const tinyusb_config_cdcacm_t cdc = {
        .usb_dev = TINYUSB_USBDEV_0,
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_line_state_changed = line_state_changed,
        .callback_line_coding_changed = line_coding_changed,
    };
    return tusb_cdc_acm_init(&cdc);
}
