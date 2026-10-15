/* =============================================================================
 * eth_w5500.h — Brain Ethernet: W5500 on SPI2, DHCP client
 * =============================================================================
 * Brings up the brain's W5500 (esp_eth raw-MAC driver under lwIP), attaches
 * it to esp_netif with DHCP, and reports when an IPv4 address is held.
 * Pins are fixed by the brain's schematic (see eth_w5500.c).
 * ============================================================================= */

#ifndef ETH_W5500_H
#define ETH_W5500_H

#include <stdbool.h>

#include "esp_err.h"
#include "esp_netif_types.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif


/* -----------------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------------- */

/**
 * Initialize the network stack, the SPI bus and the W5500, and start DHCP.
 * Call once; needs the default event loop not to exist yet.
 *
 * @return ESP_OK, or the first error from the SPI/esp_eth/esp_netif setup.
 */
esp_err_t eth_w5500_start(void);

/**
 * Block until DHCP has assigned an address, or the timeout expires.
 *
 * @return true if an address is held.
 */
bool eth_w5500_wait_for_ip(TickType_t timeout);

/**
 * Current address, netmask and gateway (all 0.0.0.0 without a lease).
 *
 * @return ESP_OK; ESP_ERR_INVALID_STATE before eth_w5500_start succeeded.
 */
esp_err_t eth_w5500_get_ip_info(esp_netif_ip_info_t *ip_info);


#ifdef __cplusplus
}
#endif

#endif /* ETH_W5500_H */
