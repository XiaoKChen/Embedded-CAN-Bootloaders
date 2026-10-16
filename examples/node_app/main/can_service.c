/* =============================================================================
 * can_service.c — The node app's side of the CAN update protocol
 * =============================================================================
 * See can_service.h. Requests and replies are encoded by the shared, host-
 * tested codec (src/boot/app_control.c); this file only moves frames.
 *
 * ENTER_UPDATER ordering: ACK first, let it drain, then
 * can_updater_client_enter_updater() selects `factory` and calls esp_restart().
 * That restart passes the client's shutdown handler, so Phase 3 recovery
 * counts it as clean, not as a crash. A lost ACK is harmless: the host's retry
 * reaches the freshly booted updater, whose NAK also means "entered".
 * ============================================================================= */

#include "can_service.h"

#include <string.h>

#include "app_control.h"
#include "can_link.h"
#include "can_updater_client.h"
#include "dfu_config.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"


/* -----------------------------------------------------------------------------
 * Constants (partition label/subtype mirror node_updater/partitions.csv)
 * ----------------------------------------------------------------------------- */
#define CAN_CFG_PARTITION_LABEL    "cancfg"
#define CAN_CFG_PARTITION_SUBTYPE  ((esp_partition_subtype_t)0x40)

#define TASK_STACK_BYTES           4096u
#define TASK_PRIORITY              5u
#define RX_POLL_TIMEOUT_MS         1000u
/* Lets the ENTER_UPDATER ACK reach the wire before can_link_deinit (which
 * itself waits up to 100 ms for the TX queue) and the restart. */
#define ENTER_ACK_DRAIN_MS         20u
#define LINK_RETRY_DELAY_MS        1000u

static const char *TAG = "can_service";


/* -----------------------------------------------------------------------------
 * File-local state
 * ----------------------------------------------------------------------------- */
static can_service_ids_t s_ids;
static StaticTask_t      s_task;
static StackType_t       s_stack[TASK_STACK_BYTES / sizeof(StackType_t)];


/* -----------------------------------------------------------------------------
 * Private helpers
 * ----------------------------------------------------------------------------- */
static bool link_start(void)
{
    const can_link_config_t cfg = {
        .tx_gpio     = CONFIG_NODE_APP_TWAI_TX_GPIO,
        .rx_gpio     = CONFIG_NODE_APP_TWAI_RX_GPIO,
        .bitrate_bps = CONFIG_NODE_APP_CAN_BITRATE_BPS,
        .accept_id   = s_ids.cmd_id,
    };
    return can_link_init(&cfg) == CAN_LINK_STATUS_OK;
}

static void send_reply(uint8_t cmd, uint8_t err)
{
    uint8_t reply[APP_CONTROL_FRAME_BYTES];
    app_control_encode_reply(cmd, err, reply);
    (void)can_link_send(s_ids.reply_id, reply);  /* the host retries on loss */
}

static void serve_app_info(void)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    uint8_t frames[APP_CONTROL_INFO_BYTES];
    app_control_encode_info(desc->version, desc->project_name, frames);
    /* 9 frames vs an 8-deep TX queue: can_link_send waits for space. A frame
     * lost here fails the host's burst check and it simply asks again. */
    for (uint32_t off = 0u; off < sizeof(frames); off += APP_CONTROL_FRAME_BYTES) {
        (void)can_link_send(s_ids.reply_id, &frames[off]);
    }
    ESP_LOGI(TAG, "APP_INFO answered: %s %s", desc->project_name, desc->version);
}

static void serve_enter_updater(void)
{
    /* Refuse up front instead of ACKing an entry that cannot happen. */
    if (esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL) == NULL) {
        ESP_LOGE(TAG, "ENTER_UPDATER refused: no factory (updater) partition");
        send_reply(CAN_UPDATE_CMD_ENTER_UPDATER, CAN_UPDATE_ERR_FLASH);
        return;
    }

    ESP_LOGW(TAG, "ENTER_UPDATER: acknowledging, then restarting into the CAN updater");
    send_reply(CAN_UPDATE_CMD_ENTER_UPDATER, CAN_UPDATE_OK);
    vTaskDelay(pdMS_TO_TICKS(ENTER_ACK_DRAIN_MS));
    can_link_deinit();

    const esp_err_t err = can_updater_client_enter_updater();  /* restarts on success */
    ESP_LOGE(TAG, "cannot enter the CAN updater: %s; app keeps running", esp_err_to_name(err));
    while (!link_start()) {
        ESP_LOGE(TAG, "TWAI restart failed; retrying");
        vTaskDelay(pdMS_TO_TICKS(LINK_RETRY_DELAY_MS));
    }
}

static void service_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t id;
        uint8_t  dlc;
        uint8_t  data[8];
        if (can_link_receive(&id, &dlc, data, RX_POLL_TIMEOUT_MS) != CAN_LINK_STATUS_OK ||
            id != s_ids.cmd_id) {
            continue;  /* timeout, or not addressed to us (hardware filters too) */
        }

        switch (app_control_parse_request(dlc, data)) {
        case APP_CONTROL_REQ_APP_INFO:
            serve_app_info();
            break;
        case APP_CONTROL_REQ_ENTER_UPDATER:
            serve_enter_updater();
            break;
        case APP_CONTROL_REQ_MALFORMED:
            ESP_LOGW(TAG, "malformed request 0x%02X (DLC %u) NAKed", data[0], (unsigned)dlc);
            send_reply(data[0], CAN_UPDATE_ERR_STATE);
            break;
        default:
            /* HELLO/INFO/... reach a running app when a host expected the
             * updater. Silence is the answer: it makes the host try
             * ENTER_UPDATER. */
            ESP_LOGI(TAG, "updater command 0x%02X ignored: this node is running its app", data[0]);
            break;
        }
    }
}


/* -----------------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------------- */
void can_service_load_ids(can_service_ids_t *ids)
{
    /* Missing partition or failed read leaves the block erased (0xFF), which
     * is exactly an unprovisioned board: the defaults apply. */
    uint8_t block[DFU_CONFIG_SIZE];
    memset(block, 0xFF, sizeof(block));
    const esp_partition_t *cfg = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, CAN_CFG_PARTITION_SUBTYPE, CAN_CFG_PARTITION_LABEL);
    if (cfg == NULL || esp_partition_read(cfg, 0u, block, sizeof(block)) != ESP_OK) {
        memset(block, 0xFF, sizeof(block));
        ESP_LOGW(TAG, "cancfg partition unreadable; using the default CAN IDs");
    }

    dfu_config_can_ids(block, &ids->cmd_id, &ids->reply_id);
    ids->custom = (ids->cmd_id != CAN_ID_HOST_TO_BOOT || ids->reply_id != CAN_ID_BOOT_TO_HOST);
}

esp_err_t can_service_start(const can_service_ids_t *ids)
{
    s_ids = *ids;
    if (!link_start()) {
        ESP_LOGE(TAG, "TWAI start failed (TX GPIO %d, RX GPIO %d, %d bit/s)",
                 CONFIG_NODE_APP_TWAI_TX_GPIO, CONFIG_NODE_APP_TWAI_RX_GPIO,
                 CONFIG_NODE_APP_CAN_BITRATE_BPS);
        return ESP_FAIL;
    }
    if (xTaskCreateStatic(service_task, "can_service", TASK_STACK_BYTES,
                          NULL, TASK_PRIORITY, s_stack, &s_task) == NULL) {
        can_link_deinit();
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "listening: cmd ID 0x%03X, reply ID 0x%03X (%s), TWAI TX GPIO %d / RX GPIO %d, %d bit/s",
             s_ids.cmd_id, s_ids.reply_id, s_ids.custom ? "per-device, cancfg" : "defaults",
             CONFIG_NODE_APP_TWAI_TX_GPIO, CONFIG_NODE_APP_TWAI_RX_GPIO,
             CONFIG_NODE_APP_CAN_BITRATE_BPS);
    return ESP_OK;
}
