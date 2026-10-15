/* =============================================================================
 * eth_w5500.c — Brain Ethernet: W5500 on SPI2, DHCP client
 * =============================================================================
 * See eth_w5500.h for the public interface. Follows ESP-IDF v5.3's
 * examples/ethernet/basic (ethernet_init.c), reduced to one W5500.
 * ============================================================================= */

#include "eth_w5500.h"

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_eth.h"
#include "esp_eth_mac_spi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "freertos/event_groups.h"


/* -----------------------------------------------------------------------------
 * Constants — the brain's W5500 wiring (SPI2 on its IOMUX pins)
 * ----------------------------------------------------------------------------- */
#define W5500_SPI_HOST        SPI2_HOST
#define W5500_MOSI_GPIO       11
#define W5500_MISO_GPIO       13
#define W5500_SCLK_GPIO       12
#define W5500_CS_GPIO         10
#define W5500_INT_GPIO        9       /* INTn, active low                    */
#define W5500_RST_GPIO        14      /* RSTn, pulsed by the PHY driver      */
#define W5500_SPI_CLOCK_HZ    (20 * 1000 * 1000)  /* conservative; W5500 max is 80 MHz */
#define W5500_SPI_QUEUE_SIZE  20
#define W5500_PHY_ADDR        1       /* internal PHY; not addressed over MDIO */
#define MAC_LEN               6u

#define GOT_IP_BIT            BIT0

static const char *TAG = "eth_w5500";


/* -----------------------------------------------------------------------------
 * File-local state
 * ----------------------------------------------------------------------------- */
static esp_netif_t        *s_netif;
static EventGroupHandle_t  s_events;


/* -----------------------------------------------------------------------------
 * Private helpers
 * ----------------------------------------------------------------------------- */
static void on_eth_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;
    if (id == ETHERNET_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "link up");
    } else if (id == ETHERNET_EVENT_DISCONNECTED) {
        ESP_LOGW(TAG, "link down");
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == IP_EVENT_ETH_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "==================================================");
        ESP_LOGI(TAG, "IP acquired: " IPSTR "   ->  http://" IPSTR "/",
                 IP2STR(&event->ip_info.ip), IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "==================================================");
        xEventGroupSetBits(s_events, GOT_IP_BIT);
    } else if (id == IP_EVENT_ETH_LOST_IP) {
        ESP_LOGW(TAG, "IP lost");
        xEventGroupClearBits(s_events, GOT_IP_BIT);
    }
}

static esp_err_t install_w5500(esp_eth_handle_t *eth)
{
    /* The W5500 driver hooks INTn through the shared GPIO ISR service. */
    const esp_err_t isr_err = gpio_install_isr_service(0);
    if (isr_err != ESP_OK && isr_err != ESP_ERR_INVALID_STATE) {  /* already installed is fine */
        return isr_err;
    }

    const spi_bus_config_t bus = {
        .mosi_io_num   = W5500_MOSI_GPIO,
        .miso_io_num   = W5500_MISO_GPIO,
        .sclk_io_num   = W5500_SCLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(W5500_SPI_HOST, &bus, SPI_DMA_CH_AUTO),
                        TAG, "SPI2 bus init failed");

    /* Copied by esp_eth_mac_new_w5500(), so a stack object is enough. */
    spi_device_interface_config_t device = {
        .mode           = 0,
        .clock_speed_hz = W5500_SPI_CLOCK_HZ,
        .queue_size     = W5500_SPI_QUEUE_SIZE,
        .spics_io_num   = W5500_CS_GPIO,
    };
    eth_w5500_config_t w5500 = ETH_W5500_DEFAULT_CONFIG(W5500_SPI_HOST, &device);
    w5500.int_gpio_num = W5500_INT_GPIO;

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr       = W5500_PHY_ADDR;
    phy_config.reset_gpio_num = W5500_RST_GPIO;

    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500, &mac_config);
    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_config);
    ESP_RETURN_ON_FALSE(mac != NULL && phy != NULL, ESP_FAIL, TAG, "W5500 MAC/PHY create failed");

    const esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
    ESP_RETURN_ON_ERROR(esp_eth_driver_install(&config, eth), TAG,
                        "W5500 driver install failed (SPI wiring? chip in reset?)");

    /* The W5500 has no factory MAC: use the chip's universally administered
     * Ethernet MAC (base MAC + 3 on the S3). */
    uint8_t mac_addr[MAC_LEN] = { 0 };
    ESP_RETURN_ON_ERROR(esp_read_mac(mac_addr, ESP_MAC_ETH), TAG, "reading the Ethernet MAC failed");
    ESP_RETURN_ON_ERROR(esp_eth_ioctl(*eth, ETH_CMD_S_MAC_ADDR, mac_addr), TAG, "setting the MAC failed");
    ESP_LOGI(TAG, "W5500 on SPI2 at %d MHz (MOSI %d, MISO %d, SCLK %d, CS %d, INT %d, RST %d), "
                  "MAC %02x:%02x:%02x:%02x:%02x:%02x",
             W5500_SPI_CLOCK_HZ / 1000000, W5500_MOSI_GPIO, W5500_MISO_GPIO, W5500_SCLK_GPIO,
             W5500_CS_GPIO, W5500_INT_GPIO, W5500_RST_GPIO,
             mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
    return ESP_OK;
}


/* -----------------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------------- */
esp_err_t eth_w5500_start(void)
{
    s_events = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_events != NULL, ESP_ERR_NO_MEM, TAG, "event group alloc failed");

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif_init failed");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "default event loop failed");

    esp_eth_handle_t eth = NULL;
    ESP_RETURN_ON_ERROR(install_w5500(&eth), TAG, "W5500 bring-up failed");

    const esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_ETH();  /* DHCP client */
    esp_netif_t *netif = esp_netif_new(&netif_config);
    ESP_RETURN_ON_FALSE(netif != NULL, ESP_ERR_NO_MEM, TAG, "esp_netif_new failed");
    esp_eth_netif_glue_handle_t glue = esp_eth_new_netif_glue(eth);
    ESP_RETURN_ON_FALSE(glue != NULL, ESP_ERR_NO_MEM, TAG, "netif glue alloc failed");
    ESP_RETURN_ON_ERROR(esp_netif_attach(netif, glue), TAG, "esp_netif_attach failed");
    s_netif = netif;

    ESP_RETURN_ON_ERROR(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, on_eth_event, NULL),
                        TAG, "ETH_EVENT handler failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_ip_event, NULL),
                        TAG, "GOT_IP handler failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_LOST_IP, on_ip_event, NULL),
                        TAG, "LOST_IP handler failed");

    ESP_RETURN_ON_ERROR(esp_eth_start(eth), TAG, "esp_eth_start failed");
    ESP_LOGI(TAG, "Ethernet started; waiting for link and DHCP");
    return ESP_OK;
}

bool eth_w5500_wait_for_ip(TickType_t timeout)
{
    if (s_events == NULL) {
        return false;
    }
    const EventBits_t bits = xEventGroupWaitBits(s_events, GOT_IP_BIT, pdFALSE, pdTRUE, timeout);
    return (bits & GOT_IP_BIT) != 0u;
}

esp_err_t eth_w5500_get_ip_info(esp_netif_ip_info_t *ip_info)
{
    if (s_netif == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_netif_get_ip_info(s_netif, ip_info);
}
