/**
 * @file    can_link.h
 * @brief   Public interface for the TWAI (classic CAN) link driver.
 *
 * Thin wrapper over the ESP-IDF v5.3 legacy TWAI driver (driver/twai.h), shared by the updater and
 * the SLCAN bridge (examples/slcan_bridge). The updater's view: one standard 11-bit RX ID filtered
 * in hardware, DLC-8 frames out (can_link_receive / can_link_send). The bridge's view: every frame
 * in and out (accept_all, can_link_*_frame).
 *
 * Owns:         The TWAI driver instance and bus-off recovery, which is handled inside
 *               receive/send either way, so callers never have to know.
 * Invariants:   No IDF types leak through this header.
 * Concurrency:  Not reentrant across init/deinit: the caller must keep every other call out while
 *               one of those two runs (deinit frees the queue a receive blocks on).
 * Errors:       Calls return can_link_status_t; no (matching) frame within the wait is
 *               CAN_LINK_STATUS_TIMEOUT.
 *
 * @see     can_link.c
 */

#ifndef CAN_LINK_H
#define CAN_LINK_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif


#define CAN_LINK_MAX_DATA_LEN  8u


typedef enum {
    CAN_LINK_STATUS_OK = 0,
    CAN_LINK_STATUS_ERROR,
    CAN_LINK_STATUS_INVALID_ARG,
    CAN_LINK_STATUS_TIMEOUT,      /* no (matching) frame within the wait */
} can_link_status_t;


typedef struct {
    int32_t  tx_gpio;      /* routed through the GPIO matrix: any output pin  */
    int32_t  rx_gpio;
    uint32_t bitrate_bps;  /* 10k, 20k, 50k, 100k, 125k, 250k, 500k or 1M     */
    uint32_t accept_id;    /* the one 11-bit standard ID passed up to receive */
    bool     accept_all;   /* pass every frame up instead (accept_id ignored) */
    bool     listen_only;  /* never ACK, never transmit                       */
    uint32_t rx_queue_len; /* frames buffered for receive; 0 = default (64)   */
} can_link_config_t;

typedef struct {
    uint32_t id;           /* 11-bit, or 29-bit when extended                  */
    uint8_t  dlc;          /* as on the wire (0..15); data holds min(dlc, 8)   */
    bool     extended;
    bool     remote;       /* RTR: no data bytes                               */
    uint8_t  data[CAN_LINK_MAX_DATA_LEN];
} can_link_frame_t;

typedef enum {
    CAN_LINK_BUS_STOPPED = 0,     /* not running (also: recovery completed)  */
    CAN_LINK_BUS_ERROR_ACTIVE,
    CAN_LINK_BUS_ERROR_PASSIVE,   /* an error counter reached 128            */
    CAN_LINK_BUS_OFF,
    CAN_LINK_BUS_RECOVERING,
} can_link_bus_state_t;

typedef struct {
    can_link_bus_state_t state;
    uint32_t tx_error_counter;
    uint32_t rx_error_counter;
    uint32_t rx_missed_count;     /* frames lost: receive queue full         */
    uint32_t rx_overrun_count;    /* frames lost: hardware RX FIFO overrun   */
} can_link_health_t;


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

/**
 * Wait up to timeout_ms for any frame the filter passes (standard, extended,
 * data or remote). Recovery on timeout as in can_link_receive.
 */
can_link_status_t can_link_receive_frame(can_link_frame_t *frame, uint32_t timeout_ms);

/** Queue one frame (dlc 0..8) for transmission; recovery on failure. */
can_link_status_t can_link_send_frame(const can_link_frame_t *frame);

/** Snapshot of the controller's error state and loss counters since init. */
can_link_status_t can_link_get_health(can_link_health_t *health);

/** Let queued frames drain (bounded wait), then stop and uninstall. */
void can_link_deinit(void);


#ifdef __cplusplus
}
#endif

#endif /* CAN_LINK_H */
