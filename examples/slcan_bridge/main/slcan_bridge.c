/* =============================================================================
 * slcan_bridge.c — USB <-> CAN adapter speaking SLCAN, for the brain board
 * =============================================================================
 * Turns the brain (ESP32-S3-WROOM-1U-N16R8, TCAN332DR on TWAI TX GPIO6 /
 * RX GPIO7) into an SLCAN (Lawicel) adapter on its native USB-Serial-JTAG
 * port, so tools/can-flash drives the node's updater unchanged:
 *   can_flash.py app.bin --interface slcan --channel COMx --bitrate 250000
 * The command subset is python-can's (components/slcan_codec).
 *
 * Two tasks on one core:
 *   usb_to_can  splits the USB byte stream into CR-terminated lines, runs
 *               them (C/O/L, Sn, t/T/r/R, V, N) and answers ACK/NAK;
 *   can_to_usb  forwards every received frame as a t/T/r/R line, and logs
 *               bus-state changes and frame loss.
 * The USB port carries SLCAN only; ESP_LOG goes to UART0 (sdkconfig.defaults).
 *
 * Buffering: python-can has no flow control, and the v5.3 USB-Serial-JTAG
 * driver drops host bytes when its RX ring is full rather than NAKing USB.
 *   USB RX ring  16 KB  one can-flash DATA window is 342 lines of 22 bytes
 *                       ("t79E8" + 16 hex + CR) = 7.5 KB, written faster than
 *                       250 kbit/s drains it (~2000 frames/s): 2x headroom.
 *   USB TX ring   8 KB  ~370 received frames while the host is not reading.
 *   TWAI RX queue 256   more frames while that ring is full; beyond that the
 *                       driver counts them as missed, and they are logged.
 * ============================================================================= */

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "can_link.h"
#include "slcan_codec.h"


/* -----------------------------------------------------------------------------
 * Constants
 * ----------------------------------------------------------------------------- */
#define USB_RX_BUFFER_BYTES   16384u
#define USB_TX_BUFFER_BYTES   8192u
#define USB_READ_CHUNK_BYTES  64u      /* one full-speed bulk packet           */
#define TWAI_RX_QUEUE_FRAMES  256u

#define RX_POLL_MS            10u      /* bounds how long C/O wait for the link */
#define CLOSED_POLL_MS        10u
#define HEALTH_POLL_MS        250u

#define NODE_EN_GPIO          3        /* brain GPIO3 -> node EN (reset)        */

#define ADAPTER_HW_VERSION    1u       /* V reply: "V0101"                      */
#define ADAPTER_SW_VERSION    1u
#define MAC_LEN               6u

#define BRIDGE_CORE           (portNUM_PROCESSORS - 1)
#define USB_TO_CAN_PRIORITY   6u       /* above can_to_usb: see s_link_mutex    */
#define CAN_TO_USB_PRIORITY   5u
#define TASK_STACK_BYTES      4096u

_Static_assert(CAN_LINK_MAX_DATA_LEN == SLCAN_MAX_DLC, "frame data sizes differ");

static const char *TAG = "slcan_bridge";


/* -----------------------------------------------------------------------------
 * File-local state
 * ----------------------------------------------------------------------------- */
/* Held around every can_link call that blocks on or tears down the TWAI
 * driver's queues (deinit frees the RX queue can_to_usb waits on). Both tasks
 * run on BRIDGE_CORE and usb_to_can has the higher priority, so a give in
 * can_to_usb hands the mutex straight over instead of re-taking it. */
static SemaphoreHandle_t s_link_mutex;
static bool              s_channel_open;   /* written by usb_to_can under the mutex */
static can_link_health_t s_last_health;    /* under the mutex                       */

/* usb_to_can only. */
static bool     s_listen_only;
static uint32_t s_bitrate_bps = CONFIG_SLCAN_BRIDGE_BITRATE_BPS;
static uint16_t s_serial;

/* Written by usb_to_can only; can_to_usb reads them for its periodic report. */
static volatile uint32_t s_malformed_lines;
static volatile uint32_t s_tx_failures;

/* can_to_usb only. */
static TickType_t s_last_health_poll;
static uint32_t   s_reported_malformed_lines;
static uint32_t   s_reported_tx_failures;


/* -----------------------------------------------------------------------------
 * USB side
 * ----------------------------------------------------------------------------- */
/* One write per line: the driver's byte ring buffer takes it whole, so lines
 * from the two tasks never interleave. Blocks while the host is not reading;
 * with portMAX_DELAY it can only fail for a line longer than the ring. */
static void usb_write(const char *text, size_t len)
{
    (void)usb_serial_jtag_write_bytes(text, len, portMAX_DELAY);
}

static void usb_write_str(const char *text)
{
    usb_write(text, strlen(text));
}

static void usb_ack_or_nak(bool ok)
{
    usb_write_str(ok ? SLCAN_ACK : SLCAN_NAK);
}


/* -----------------------------------------------------------------------------
 * CAN channel (usb_to_can)
 * ----------------------------------------------------------------------------- */
static bool open_channel(bool listen_only)
{
    const can_link_config_t cfg = {
        .tx_gpio      = CONFIG_SLCAN_BRIDGE_TWAI_TX_GPIO,
        .rx_gpio      = CONFIG_SLCAN_BRIDGE_TWAI_RX_GPIO,
        .bitrate_bps  = s_bitrate_bps,
        .accept_all   = true,
        .listen_only  = listen_only,
        .rx_queue_len = TWAI_RX_QUEUE_FRAMES,
    };

    (void)xSemaphoreTake(s_link_mutex, portMAX_DELAY);
    const bool opened = (can_link_init(&cfg) == CAN_LINK_STATUS_OK);
    if (opened) {
        s_channel_open = true;
        memset(&s_last_health, 0, sizeof(s_last_health));
        s_last_health.state = CAN_LINK_BUS_ERROR_ACTIVE;
    }
    (void)xSemaphoreGive(s_link_mutex);

    if (!opened) {
        ESP_LOGE(TAG, "cannot open at %" PRIu32 " bit/s (unsupported bitrate, or TWAI "
                      "install failed)", s_bitrate_bps);
        return false;
    }
    s_listen_only = listen_only;
    ESP_LOGI(TAG, "channel open at %" PRIu32 " bit/s%s", s_bitrate_bps,
             listen_only ? ", listen-only" : "");
    return true;
}

static void close_channel(void)
{
    (void)xSemaphoreTake(s_link_mutex, portMAX_DELAY);
    const bool was_open = s_channel_open;
    if (was_open) {
        can_link_deinit();
        s_channel_open = false;
    }
    (void)xSemaphoreGive(s_link_mutex);

    if (was_open) {
        ESP_LOGI(TAG, "channel closed");
    }
}

static void transmit(const slcan_frame_t *frame)
{
    if (!s_channel_open || s_listen_only) {
        usb_write_str(SLCAN_NAK);
        return;
    }

    can_link_frame_t out = {
        .id       = frame->id,
        .dlc      = frame->dlc,
        .extended = frame->extended,
        .remote   = frame->remote,
    };
    memcpy(out.data, frame->data, sizeof(out.data));

    if (can_link_send_frame(&out) != CAN_LINK_STATUS_OK) {
        s_tx_failures++;
        usb_write_str(SLCAN_NAK);
        return;
    }
    usb_write_str(frame->extended ? SLCAN_TX_ACK_EXT : SLCAN_TX_ACK_STD);
}

static void reply_version(void)
{
    char   reply[SLCAN_REPLY_LINE_MAX];
    size_t len;
    if (slcan_encode_version(ADAPTER_HW_VERSION, ADAPTER_SW_VERSION, reply, sizeof(reply),
                             &len) != SLCAN_STATUS_OK) {
        usb_write_str(SLCAN_NAK);
        return;
    }
    usb_write(reply, len);
}

static void reply_serial(void)
{
    char   reply[SLCAN_REPLY_LINE_MAX];
    size_t len;
    if (slcan_encode_serial(s_serial, reply, sizeof(reply), &len) != SLCAN_STATUS_OK) {
        usb_write_str(SLCAN_NAK);
        return;
    }
    usb_write(reply, len);
}

/* Lawicel semantics: Sn only while closed; C always succeeds. O/L on an open
 * channel succeed when the mode matches: python-can sends O twice on every
 * open (set_bitrate() reopens, then __init__ calls open() again). */
static void run_command(const slcan_command_t *cmd)
{
    switch (cmd->type) {
    case SLCAN_CMD_EMPTY:
        usb_write_str(SLCAN_ACK);
        break;
    case SLCAN_CMD_CLOSE:
        close_channel();
        usb_write_str(SLCAN_ACK);
        break;
    case SLCAN_CMD_SET_BITRATE:
        if (!s_channel_open) {
            s_bitrate_bps = cmd->bitrate_bps;
        }
        usb_ack_or_nak(!s_channel_open);
        break;
    case SLCAN_CMD_OPEN:
    case SLCAN_CMD_LISTEN: {
        const bool listen_only = (cmd->type == SLCAN_CMD_LISTEN);
        usb_ack_or_nak(s_channel_open ? (s_listen_only == listen_only)
                                      : open_channel(listen_only));
        break;
    }
    case SLCAN_CMD_TRANSMIT:
        transmit(&cmd->frame);
        break;
    case SLCAN_CMD_VERSION:
        reply_version();
        break;
    case SLCAN_CMD_SERIAL:
        reply_serial();
        break;
    default:
        usb_write_str(SLCAN_NAK);
        break;
    }
}

static void handle_line(const char *line, size_t len, bool overlong)
{
    slcan_command_t cmd;
    if (overlong || slcan_parse_line(line, len, &cmd) != SLCAN_STATUS_OK) {
        s_malformed_lines++;
        usb_write_str(SLCAN_NAK);
        return;
    }
    run_command(&cmd);
}

static void usb_to_can_task(void *arg)
{
    (void)arg;
    char   line[SLCAN_LINE_MAX];
    size_t len      = 0u;
    bool   overlong = false;

    for (;;) {
        uint8_t chunk[USB_READ_CHUNK_BYTES];
        const int received = usb_serial_jtag_read_bytes(chunk, sizeof(chunk), portMAX_DELAY);

        for (int i = 0; i < received; i++) {
            const char c = (char)chunk[i];
            if (c == '\n') {
                continue;  /* tolerate CR LF from terminals */
            }
            if (c != '\r') {
                if (len < sizeof(line)) {
                    line[len++] = c;
                } else {
                    overlong = true;
                }
                continue;
            }
            handle_line(line, len, overlong);
            len      = 0u;
            overlong = false;
        }
    }
}


/* -----------------------------------------------------------------------------
 * CAN -> USB (can_to_usb)
 * ----------------------------------------------------------------------------- */
static const char *bus_state_name(can_link_bus_state_t state)
{
    switch (state) {
    case CAN_LINK_BUS_ERROR_ACTIVE:  return "error-active";
    case CAN_LINK_BUS_ERROR_PASSIVE: return "error-passive";
    case CAN_LINK_BUS_OFF:           return "bus-off, recovering";
    case CAN_LINK_BUS_RECOVERING:    return "recovering";
    case CAN_LINK_BUS_STOPPED:       return "recovered, restarting";
    default:                         return "?";
    }
}

/* Called with s_link_mutex held and the channel open. Logs go to UART0. */
static void report_link_health(void)
{
    const TickType_t now = xTaskGetTickCount();
    if ((now - s_last_health_poll) < pdMS_TO_TICKS(HEALTH_POLL_MS)) {
        return;
    }
    s_last_health_poll = now;

    can_link_health_t health;
    if (can_link_get_health(&health) != CAN_LINK_STATUS_OK) {
        return;
    }
    if (health.state != s_last_health.state) {
        ESP_LOGW(TAG, "bus %s (TEC %" PRIu32 ", REC %" PRIu32 ")",
                 bus_state_name(health.state), health.tx_error_counter,
                 health.rx_error_counter);
    }
    if (health.rx_missed_count != s_last_health.rx_missed_count) {
        ESP_LOGW(TAG, "%" PRIu32 " received frame(s) lost: TWAI RX queue full "
                      "(host not reading the USB port?)",
                 health.rx_missed_count - s_last_health.rx_missed_count);
    }
    if (health.rx_overrun_count != s_last_health.rx_overrun_count) {
        ESP_LOGW(TAG, "%" PRIu32 " received frame(s) lost: TWAI RX FIFO overrun",
                 health.rx_overrun_count - s_last_health.rx_overrun_count);
    }
    s_last_health = health;

    const uint32_t malformed = s_malformed_lines;
    if (malformed != s_reported_malformed_lines) {
        ESP_LOGW(TAG, "%" PRIu32 " malformed SLCAN line(s) NAKed (mid-transfer: USB RX "
                      "ring overflow)", malformed - s_reported_malformed_lines);
        s_reported_malformed_lines = malformed;
    }
    const uint32_t tx_failures = s_tx_failures;
    if (tx_failures != s_reported_tx_failures) {
        ESP_LOGW(TAG, "%" PRIu32 " frame(s) not sent: TWAI TX queue stayed full, or bus-off",
                 tx_failures - s_reported_tx_failures);
        s_reported_tx_failures = tx_failures;
    }
}

static void forward_frame(const can_link_frame_t *frame)
{
    slcan_frame_t out = {
        .id       = frame->id,
        .dlc      = frame->dlc,
        .extended = frame->extended,
        .remote   = frame->remote,
    };
    memcpy(out.data, frame->data, sizeof(out.data));

    char   line[SLCAN_FRAME_LINE_MAX];
    size_t len;
    if (slcan_encode_frame(&out, line, sizeof(line), &len) == SLCAN_STATUS_OK) {
        usb_write(line, len);
    }
}

static void can_to_usb_task(void *arg)
{
    (void)arg;
    for (;;) {
        can_link_frame_t  frame;
        can_link_status_t status = CAN_LINK_STATUS_TIMEOUT;

        (void)xSemaphoreTake(s_link_mutex, portMAX_DELAY);
        const bool open = s_channel_open;
        if (open) {
            status = can_link_receive_frame(&frame, RX_POLL_MS);
            report_link_health();
        }
        (void)xSemaphoreGive(s_link_mutex);

        if (!open) {
            vTaskDelay(pdMS_TO_TICKS(CLOSED_POLL_MS));
        } else if (status == CAN_LINK_STATUS_OK) {
            forward_frame(&frame);  /* outside the mutex: may block on USB */
        }
    }
}


/* -----------------------------------------------------------------------------
 * Board bring-up
 * ----------------------------------------------------------------------------- */
/* GPIO3 drives the node's reset through an inverting stage: HIGH holds the
 * node in reset, LOW lets it run. A floating GPIO3 can read as HIGH, so this
 * firmware drives it LOW and never resets the node. The level is latched
 * before the output is enabled so the pin never glitches HIGH. */
static esp_err_t release_node_en(void)
{
    const esp_err_t err = gpio_set_level(NODE_EN_GPIO, 0);
    if (err != ESP_OK) {
        return err;
    }
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << NODE_EN_GPIO,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    return gpio_config(&cfg);
}

static uint16_t adapter_serial(void)
{
    uint8_t mac[MAC_LEN] = { 0 };
    if (esp_efuse_mac_get_default(mac) != ESP_OK) {
        return 0u;  /* N then answers "N0000"; nothing depends on it */
    }
    return (uint16_t)(((uint16_t)mac[4] << 8) | mac[5]);
}


/* -----------------------------------------------------------------------------
 * Entry
 * ----------------------------------------------------------------------------- */
void app_main(void)
{
    ESP_ERROR_CHECK(release_node_en());

    usb_serial_jtag_driver_config_t usb_cfg = {
        .tx_buffer_size = USB_TX_BUFFER_BYTES,
        .rx_buffer_size = USB_RX_BUFFER_BYTES,
    };
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb_cfg));

    s_serial     = adapter_serial();
    s_link_mutex = xSemaphoreCreateMutex();
    if (s_link_mutex == NULL ||
        xTaskCreatePinnedToCore(usb_to_can_task, "usb_to_can", TASK_STACK_BYTES, NULL,
                                USB_TO_CAN_PRIORITY, NULL, BRIDGE_CORE) != pdPASS ||
        xTaskCreatePinnedToCore(can_to_usb_task, "can_to_usb", TASK_STACK_BYTES, NULL,
                                CAN_TO_USB_PRIORITY, NULL, BRIDGE_CORE) != pdPASS) {
        ESP_LOGE(TAG, "out of memory starting the bridge");
        abort();
    }

    ESP_LOGI(TAG, "SLCAN on USB-Serial-JTAG (serial N%04X); TWAI TX GPIO%d, RX GPIO%d, "
                  "%d bit/s until the host sends Sn; GPIO%d (node reset) driven LOW",
             (unsigned)s_serial, CONFIG_SLCAN_BRIDGE_TWAI_TX_GPIO,
             CONFIG_SLCAN_BRIDGE_TWAI_RX_GPIO, CONFIG_SLCAN_BRIDGE_BITRATE_BPS, NODE_EN_GPIO);
}
