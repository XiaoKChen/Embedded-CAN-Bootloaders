/* =============================================================================
 * can_polled.h — Polling-only CAN0 driver for the Renesas RA4M1 (UNO R4 Minima)
 * =============================================================================
 * Bootloader-grade driver: no interrupts, no heap, no static init order
 * dependencies. Pins D4 = P103 = CTX0 (TX), D5 = P102 = CRX0 (RX).
 * 11-bit standard IDs only. CAN clock source is PCLKB, which must run at
 * 24 MHz (board default: HOCO 48 MHz, PCLKA 48 / PCLKB 24, ratio 2:1 as
 * required for CAN on this part).
 * ============================================================================= */

#ifndef CAN_POLLED_H
#define CAN_POLLED_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CAN_POLLED_OK = 0,
    CAN_POLLED_NO_DATA,   /* receive: RX FIFO empty                            */
    CAN_POLLED_TIMEOUT,   /* send: SENTDATA never set within the loop bound    */
    CAN_POLLED_BUS_OFF,   /* bus-off detected; driver re-initialized itself —
                             caller must keep waiting / retry                  */
    CAN_POLLED_ERROR,     /* bad argument (bitrate, ID, dlc) or mode-switch
                             failure                                           */
} can_polled_status_t;

/* Bring up CAN0 at 250000 or 500000 bit/s (16 tq/bit, sample point 75 %).
 * Any other bitrate is rejected with CAN_POLLED_ERROR. Acceptance filtering
 * is exact: only frames whose 11-bit standard ID equals rx_filter_id are
 * received (4-deep hardware RX FIFO, mailboxes 28–31). */
can_polled_status_t can_polled_init(uint32_t bitrate_bps, uint32_t rx_filter_id);

/* Non-blocking. Returns CAN_POLLED_NO_DATA when the RX FIFO is empty.
 * On CAN_POLLED_OK: *id = 11-bit standard ID, *dlc = 0..8, data[0..*dlc-1]
 * filled. */
can_polled_status_t can_polled_receive(uint32_t *id, uint8_t *dlc, uint8_t data[8]);

/* Transmit one frame from a single TX mailbox and poll SENTDATA to
 * completion with a bounded loop count (returns CAN_POLLED_TIMEOUT and
 * aborts the request if the bound is hit). */
can_polled_status_t can_polled_send(uint32_t id, uint8_t dlc, const uint8_t data[8]);

/* Module-stop CAN0 and restore P102/P103 PFS to reset state (GPIO input).
 * Call before jump-to-app. */
void can_polled_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* CAN_POLLED_H */
