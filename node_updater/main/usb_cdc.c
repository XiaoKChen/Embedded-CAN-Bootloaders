/**
 * @file    usb_cdc.c
 * @brief   Provisioned USB CDC and ROM-download touch (see usb_cdc.h).
 *
 * Implementation notes: USB
 * strings are copied out of the length-delimited v2 block into static buffers.
 * UART0 remains the console: CDC never blocks CAN service or log producers.
 * The 1200-baud touch hands the USB PHY back to USB-Serial-JTAG and waits for
 * the host's bus reset before forcing ROM download, so the ROM port enumerates.
 *
 * @see     usb_cdc.h
 */
#include "usb_cdc.h"

#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_intr_alloc.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/clk_gate_ll.h"
#include "hal/usb_serial_jtag_ll.h"
#include "soc/periph_defs.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/soc.h"
#include "soc/usb_pins.h"
#include "soc/usb_serial_jtag_reg.h"
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
#define USB_JTAG_BUS_RESET_WAIT_MS 1000u  /* Arduino-ESP32 waits the same */
#define USB_TOUCH_TASK_STACK_BYTES 3072u
#define USB_TOUCH_TASK_PRIORITY    (configMAX_PRIORITIES - 2)  /* run promptly, below IDF's top tasks */

static StaticTask_t s_touch_task_tcb;
static StackType_t  s_touch_task_stack[USB_TOUCH_TASK_STACK_BYTES / sizeof(StackType_t)];
static TaskHandle_t s_touch_task;

static char s_manufacturer[DFU_CONFIG_STR_MAX + 1u] = "Normal Corporation";
static char s_product[DFU_CONFIG_STR_MAX + 1u] = "CAN Updater";
static char s_serial[USB_SERIAL_LENGTH + 1u];
static const char *s_strings[] = {
    "\x09\x04", s_manufacturer, s_product, s_serial, "CAN Updater CDC",
};

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

static void jtag_bus_reset_isr(void *arg)
{
    TaskHandle_t waiter = (TaskHandle_t)arg;
    BaseType_t woken = pdFALSE;
    const uint32_t status = usb_serial_jtag_ll_get_intsts_mask();
    usb_serial_jtag_ll_clr_intsts_mask(status);
    if ((status & USB_SERIAL_JTAG_INTR_BUS_RESET) != 0u) {
        vTaskNotifyGiveFromISR(waiter, &woken);
    }
    portYIELD_FROM_ISR(woken);
}

/* The ROM download port is the USB-Serial-JTAG controller, but TinyUSB left the
 * PHY routed to USB-OTG and a software reset keeps that routing: the ROM would
 * then sit unreachable behind a stale OTG device (issue #24). Hand the PHY back
 * and let the host re-enumerate first, as Arduino-ESP32's usb_switch_to_cdc_jtag()
 * does (cores/esp32/esp32-hal-tinyusb.c). Registers: ESP32-S3 TRM, RTC_CNTL_USB_CONF
 * and USB_SERIAL_JTAG_CONF0. */
static void restart_into_rom_download(void)
{
    /* Deleting the PHY also releases the USB_WRAP pads and pull-ups, which
     * esp_restart() does not reset (Arduino's deinit_usb_hal() step). */
    (void)tinyusb_driver_uninstall();  /* best effort: the restart below happens anyway */
    periph_ll_reset(PERIPH_USB_MODULE);
    periph_ll_disable_clk_set_rst(PERIPH_USB_MODULE);

    CLEAR_PERI_REG_MASK(RTC_CNTL_USB_CONF_REG,
                        RTC_CNTL_SW_HW_USB_PHY_SEL | RTC_CNTL_SW_USB_PHY_SEL | RTC_CNTL_USB_PAD_ENABLE);
    CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_PHY_SEL);
    CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_USB_PAD_ENABLE);

    /* D-/D+ low: the host sees a disconnect and resets the bus on reconnect. */
    const gpio_config_t usb_pins = {
        .pin_bit_mask = (1ull << USBPHY_DM_NUM) | (1ull << USBPHY_DP_NUM),
        .mode = GPIO_MODE_OUTPUT_OD,
    };
    (void)gpio_config(&usb_pins);  /* best effort: the restart below happens anyway */
    (void)gpio_set_level(USBPHY_DM_NUM, 0);
    (void)gpio_set_level(USBPHY_DP_NUM, 0);

    /* gpio_config() on D+ (gpio_ll_pullup_dis) forces USB_SERIAL_JTAG_PAD_PULL_OVERRIDE
     * with DP_PULLUP off, and esp_restart() keeps it: the ROM would never pull D+ up.
     * Restore the default D+ pull-up and drop the override, as Arduino-ESP32 does. */
    const usb_serial_jtag_pull_override_vals_t default_pulls = {.dp_pu = 1};
    usb_serial_jtag_ll_phy_enable_pull_override(&default_pulls);
    usb_serial_jtag_ll_phy_disable_pull_override();

    usb_serial_jtag_ll_disable_intr_mask(USB_SERIAL_JTAG_LL_INTR_MASK);
    usb_serial_jtag_ll_clr_intsts_mask(USB_SERIAL_JTAG_LL_INTR_MASK);
    usb_serial_jtag_ll_ena_intr_mask(USB_SERIAL_JTAG_INTR_BUS_RESET);
    intr_handle_t bus_reset_intr = NULL;
    const bool waiting = esp_intr_alloc(ETS_USB_SERIAL_JTAG_INTR_SOURCE, 0, jtag_bus_reset_isr,
                                        xTaskGetCurrentTaskHandle(), &bus_reset_intr) == ESP_OK;

    SET_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_USB_PAD_ENABLE);

    if (waiting) {
        /* No bus reset within the timeout (e.g. no host): restart anyway. */
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(USB_JTAG_BUS_RESET_WAIT_MS));
        usb_serial_jtag_ll_disable_intr_mask(USB_SERIAL_JTAG_LL_INTR_MASK);
        (void)esp_intr_free(bus_reset_intr);
    }

    /* ESP32-S3 TRM RTC_CNTL_OPTION1: software strap into the ROM loader.
     * No flash/otadata write: interruption leaves the updater bootable. */
    REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    esp_restart();
}

/* Uninstalling TinyUSB stops the TinyUSB task, so the touch cannot run in the
 * line-coding callback that task invokes; it runs here instead. */
static void touch_task(void *arg)
{
    (void)arg;
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    restart_into_rom_download();
}

/* Same trigger as Arduino-ESP32's TinyUSB CDC: the host selecting 1200 baud is
 * the touch, whatever DTR does, so the documented "open at 1200 with DTR low,
 * then close" recipe and esptool/IDE resets both work. */
static void line_coding_changed(int itf, cdcacm_event_t *event)
{
    (void)itf;
    if (event->line_coding_changed_data.p_line_coding->bit_rate == USB_TOUCH_BAUD &&
        s_touch_task != NULL) {
        xTaskNotifyGive(s_touch_task);
    }
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
    s_touch_task = xTaskCreateStatic(touch_task, "usb_touch", sizeof(s_touch_task_stack), NULL,
                                     USB_TOUCH_TASK_PRIORITY, s_touch_task_stack, &s_touch_task_tcb);
    const tinyusb_config_cdcacm_t cdc = {
        .usb_dev = TINYUSB_USBDEV_0,
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_line_coding_changed = line_coding_changed,
    };
    return tusb_cdc_acm_init(&cdc);
}
