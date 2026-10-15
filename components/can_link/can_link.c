/* =============================================================================
 * can_link.c — TWAI link driver over the ESP-IDF v5.3 legacy TWAI API
 * =============================================================================
 * The driver's own ISR fills an RX queue; this module only blocks on that
 * queue, so no ISR lives here. Hardware filtering (single-filter mode, the
 * 11-bit ID compared, RTR and data bytes don't-care) keeps unrelated bus
 * traffic from ever reaching the RX queue, unless accept_all opens it up.
 * ============================================================================= */

#include "can_link.h"

#include <string.h>

#include "driver/twai.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


/* -----------------------------------------------------------------------------
 * Tuning
 * ----------------------------------------------------------------------------- */
#define CAN_LINK_DLC              8u
#define CAN_LINK_STD_ID_MAX       0x7FFu
#define CAN_LINK_EXT_ID_MAX       0x1FFFFFFFu
#define CAN_LINK_FILTER_ID_SHIFT  21u     /* single filter: ID in bits 31..21  */
#define CAN_LINK_RX_QUEUE_LEN     64u     /* host bursts a 342-frame window    */
#define CAN_LINK_TX_QUEUE_LEN     8u      /* INFO answers with 5 frames        */
#define CAN_LINK_TX_WAIT_MS       50u     /* TX queue full -> give up, recover */
#define CAN_LINK_DRAIN_WAIT_MS    100u    /* deinit waits this long for TX     */
#define CAN_LINK_ERROR_PASSIVE_AT 128u    /* ISO 11898-1 error-passive limit   */


/* -----------------------------------------------------------------------------
 * Private helpers
 * ----------------------------------------------------------------------------- */
static bool timing_for_bitrate(uint32_t bitrate_bps, twai_timing_config_t *out)
{
    switch (bitrate_bps) {
    case 10000u:  { const twai_timing_config_t t = TWAI_TIMING_CONFIG_10KBITS();  *out = t; return true; }
    case 20000u:  { const twai_timing_config_t t = TWAI_TIMING_CONFIG_20KBITS();  *out = t; return true; }
    case 50000u:  { const twai_timing_config_t t = TWAI_TIMING_CONFIG_50KBITS();  *out = t; return true; }
    case 100000u: { const twai_timing_config_t t = TWAI_TIMING_CONFIG_100KBITS(); *out = t; return true; }
    case 125000u: { const twai_timing_config_t t = TWAI_TIMING_CONFIG_125KBITS(); *out = t; return true; }
    case 250000u: { const twai_timing_config_t t = TWAI_TIMING_CONFIG_250KBITS(); *out = t; return true; }
    case 500000u: { const twai_timing_config_t t = TWAI_TIMING_CONFIG_500KBITS(); *out = t; return true; }
    case 1000000u: { const twai_timing_config_t t = TWAI_TIMING_CONFIG_1MBITS(); *out = t; return true; }
    default:
        return false;
    }
}

/* Bus-off (e.g. transceiver unpowered, or alone on the bus) is not fatal: run
 * the 128 x 11 recessive-bit recovery, then restart once it has completed. */
static void recover_bus_if_needed(void)
{
    twai_status_info_t info;
    if (twai_get_status_info(&info) != ESP_OK) {
        return;
    }
    if (info.state == TWAI_STATE_BUS_OFF) {
        (void)twai_initiate_recovery();  /* progress is re-checked next call */
    } else if (info.state == TWAI_STATE_STOPPED) {
        (void)twai_start();              /* recovery finished                */
    }
}

static can_link_bus_state_t bus_state_of(const twai_status_info_t *info)
{
    switch (info->state) {
    case TWAI_STATE_BUS_OFF:    return CAN_LINK_BUS_OFF;
    case TWAI_STATE_RECOVERING: return CAN_LINK_BUS_RECOVERING;
    case TWAI_STATE_RUNNING:
        return (info->tx_error_counter >= CAN_LINK_ERROR_PASSIVE_AT ||
                info->rx_error_counter >= CAN_LINK_ERROR_PASSIVE_AT)
                   ? CAN_LINK_BUS_ERROR_PASSIVE
                   : CAN_LINK_BUS_ERROR_ACTIVE;
    case TWAI_STATE_STOPPED:
    default:
        return CAN_LINK_BUS_STOPPED;
    }
}


/* -----------------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------------- */
can_link_status_t can_link_init(const can_link_config_t *cfg)
{
    if (cfg == NULL || cfg->accept_id > CAN_LINK_STD_ID_MAX) {
        return CAN_LINK_STATUS_INVALID_ARG;
    }

    twai_timing_config_t timing;
    if (!timing_for_bitrate(cfg->bitrate_bps, &timing)) {
        return CAN_LINK_STATUS_INVALID_ARG;
    }

    twai_general_config_t general = TWAI_GENERAL_CONFIG_DEFAULT(
        (gpio_num_t)cfg->tx_gpio, (gpio_num_t)cfg->rx_gpio,
        cfg->listen_only ? TWAI_MODE_LISTEN_ONLY : TWAI_MODE_NORMAL);
    general.rx_queue_len = (cfg->rx_queue_len != 0u) ? cfg->rx_queue_len
                                                     : CAN_LINK_RX_QUEUE_LEN;
    general.tx_queue_len = CAN_LINK_TX_QUEUE_LEN;

    const twai_filter_config_t accept_all = TWAI_FILTER_CONFIG_ACCEPT_ALL();
    const twai_filter_config_t accept_one = {
        .acceptance_code = cfg->accept_id << CAN_LINK_FILTER_ID_SHIFT,
        .acceptance_mask = ~(CAN_LINK_STD_ID_MAX << CAN_LINK_FILTER_ID_SHIFT),
        .single_filter   = true,
    };
    const twai_filter_config_t *filter = cfg->accept_all ? &accept_all : &accept_one;

    if (twai_driver_install(&general, &timing, filter) != ESP_OK) {
        return CAN_LINK_STATUS_ERROR;
    }
    if (twai_start() != ESP_OK) {
        (void)twai_driver_uninstall();
        return CAN_LINK_STATUS_ERROR;
    }
    return CAN_LINK_STATUS_OK;
}


can_link_status_t can_link_receive_frame(can_link_frame_t *frame, uint32_t timeout_ms)
{
    if (frame == NULL) {
        return CAN_LINK_STATUS_INVALID_ARG;
    }

    twai_message_t msg;
    if (twai_receive(&msg, pdMS_TO_TICKS(timeout_ms)) != ESP_OK) {
        recover_bus_if_needed();
        return CAN_LINK_STATUS_TIMEOUT;
    }

    memset(frame, 0, sizeof(*frame));
    frame->id       = msg.identifier;
    frame->dlc      = msg.data_length_code;
    frame->extended = (msg.extd != 0u);
    frame->remote   = (msg.rtr != 0u);
    if (!frame->remote) {
        const uint8_t len = (msg.data_length_code < CAN_LINK_DLC) ? msg.data_length_code
                                                                  : CAN_LINK_DLC;
        memcpy(frame->data, msg.data, len);
    }
    return CAN_LINK_STATUS_OK;
}


can_link_status_t can_link_receive(uint32_t *id, uint8_t *dlc, uint8_t data[8],
                                   uint32_t timeout_ms)
{
    can_link_frame_t frame;
    const can_link_status_t status = can_link_receive_frame(&frame, timeout_ms);
    if (status != CAN_LINK_STATUS_OK) {
        return status;
    }
    if (frame.extended || frame.remote) {
        return CAN_LINK_STATUS_TIMEOUT;  /* not a protocol frame */
    }

    memcpy(data, frame.data, CAN_LINK_DLC);
    *id  = frame.id;
    *dlc = frame.dlc;
    return CAN_LINK_STATUS_OK;
}


can_link_status_t can_link_send_frame(const can_link_frame_t *frame)
{
    if (frame == NULL || frame->dlc > CAN_LINK_DLC ||
        frame->id > (frame->extended ? CAN_LINK_EXT_ID_MAX : CAN_LINK_STD_ID_MAX)) {
        return CAN_LINK_STATUS_INVALID_ARG;
    }

    twai_message_t msg = {
        .extd             = frame->extended ? 1u : 0u,
        .rtr              = frame->remote ? 1u : 0u,
        .identifier       = frame->id,
        .data_length_code = frame->dlc,
    };
    if (!frame->remote) {
        memcpy(msg.data, frame->data, frame->dlc);
    }

    if (twai_transmit(&msg, pdMS_TO_TICKS(CAN_LINK_TX_WAIT_MS)) != ESP_OK) {
        recover_bus_if_needed();
        return CAN_LINK_STATUS_ERROR;
    }
    return CAN_LINK_STATUS_OK;
}


can_link_status_t can_link_send(uint32_t id, const uint8_t data[8])
{
    can_link_frame_t frame = {
        .id  = id,
        .dlc = CAN_LINK_DLC,
    };
    memcpy(frame.data, data, CAN_LINK_DLC);
    return can_link_send_frame(&frame);
}


can_link_status_t can_link_get_health(can_link_health_t *health)
{
    if (health == NULL) {
        return CAN_LINK_STATUS_INVALID_ARG;
    }

    twai_status_info_t info;
    if (twai_get_status_info(&info) != ESP_OK) {
        return CAN_LINK_STATUS_ERROR;
    }
    health->state            = bus_state_of(&info);
    health->tx_error_counter = info.tx_error_counter;
    health->rx_error_counter = info.rx_error_counter;
    health->rx_missed_count  = info.rx_missed_count;
    health->rx_overrun_count = info.rx_overrun_count;
    return CAN_LINK_STATUS_OK;
}


void can_link_deinit(void)
{
    /* Give the last reply (the GO ACK) time to leave before the reset;
     * the host would otherwise retry GO against the freshly booted app. */
    const TickType_t drain_ticks = pdMS_TO_TICKS(CAN_LINK_DRAIN_WAIT_MS);
    for (TickType_t waited = 0u; waited < drain_ticks; waited++) {
        twai_status_info_t info;
        if (twai_get_status_info(&info) != ESP_OK || info.msgs_to_tx == 0u) {
            break;
        }
        vTaskDelay(1u);  /* one tick; pdMS_TO_TICKS(1) is 0 at 100 Hz */
    }
    (void)twai_stop();
    (void)twai_driver_uninstall();
}
