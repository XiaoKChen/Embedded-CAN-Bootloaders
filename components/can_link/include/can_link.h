/* =============================================================================
 * can_link.h — Public interface for the TWAI (classic CAN) link driver
 * =============================================================================
 * Thin wrapper over the ESP-IDF v5.3 legacy TWAI driver (driver/twai.h) for
 * the updater's needs only: one standard 11-bit RX ID filtered in hardware,
 * DLC-8 frames out, bus-off recovery handled inside receive/send so the
 * caller's loop never has to know. No IDF types leak through this header.
 * ============================================================================= */

#ifndef CAN_LINK_H
#define CAN_LINK_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif


typedef enum {
    CAN_LINK_STATUS_OK = 0,
    CAN_LINK_STATUS_ERROR,
    CAN_LINK_STATUS_INVALID_ARG,
    CAN_LINK_STATUS_TIMEOUT,      /* no (standard data) frame within the wait */
} can_link_status_t;


typedef struct {
    int32_t  tx_gpio;      /* routed through the GPIO matrix: any output pin  */
    int32_t  rx_gpio;
    uint32_t bitrate_bps;  /* 125000, 250000, 500000 or 1000000               */
    uint32_t accept_id;    /* the one 11-bit standard ID passed up to receive */
} can_link_config_t;


can_link_status_t can_link_init(const can_link_config_t *cfg);

/**
 * Wait up to timeout_ms for one standard-ID data frame. Extended and remote
 * frames are dropped (reported as CAN_LINK_STATUS_TIMEOUT). On a timeout the
 * bus state is checked and a bus-off controller is put through recovery.
 */
can_link_status_t can_link_receive(uint32_t *id, uint8_t *dlc, uint8_t data[8],
                                   uint32_t timeout_ms);

/** Queue one DLC-8 standard-ID data frame for transmission. */
can_link_status_t can_link_send(uint32_t id, const uint8_t data[8]);

/** Let queued frames drain (bounded wait), then stop and uninstall. */
void can_link_deinit(void);


#ifdef __cplusplus
}
#endif

#endif /* CAN_LINK_H */
