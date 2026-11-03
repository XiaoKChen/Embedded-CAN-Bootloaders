/**
 * @file    can_polled.h
 * @brief   Polling-only CAN0 driver for the Renesas RA4M1 (UNO R4 Minima).
 *
 * Bootloader-grade driver: no interrupts, no heap, no static init order
 * dependencies. 11-bit standard IDs only.
 *
 * Owns:         CAN0 and its pins D4 = P103 = CTX0 (TX), D5 = P102 = CRX0
 *               (RX), from can_polled_init() until can_polled_deinit().
 *               Enables the DWT cycle counter as its time base and leaves it
 *               running (the driver never uses board_millis()).
 * Not here:     The CAN Update Protocol (can_update.h); the CAN service loop
 *               (main.c).
 * Invariants:   CAN clock source is PCLKB, which must run at 24 MHz (board
 *               default: HOCO 48 MHz, PCLKA 48 / PCLKB 24, ratio 2:1 as
 *               required for CAN on this part).
 *               Bit timing: 16 tq/bit, TSEG1 11, TSEG2 4, SJW 4 tq, sample
 *               point 75 %. By the ISO 11898-1 rule each node's clock may
 *               then deviate by min(SJW / (20 * 16), TSEG2 / (2 * (13 * 16 -
 *               TSEG2))) = min(1.25 %, 0.98 %) = 0.98 % from nominal (SJW 1
 *               allowed only 0.31 %). The HOCO is the only clock on this
 *               board; its datasheet accuracy (about +/-1 % over temperature,
 *               not in this repo) uses most of that budget, so the peer
 *               needs a crystal clock.
 *               FSP's r_can refuses a PCLKB-clocked CAN unless the PLL is the
 *               system clock (reference/r_can.c:233-239, "Clock Setting" in
 *               the HW manual). The uno_r4 BSP runs on the HOCO with no PLL,
 *               so this driver deliberately clocks CAN from the HOCO-derived
 *               PCLKB and relies on the SJW margin above.
 * Errors:       Every call but can_polled_deinit() returns
 *               can_polled_status_t.
 * Depends on:   The vendored RA4M1 device headers (vendor/ra4m1-headers/).
 *
 * @see     can_polled.c
 */

#ifndef CAN_POLLED_H
#define CAN_POLLED_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CAN_POLLED_OK = 0,
    CAN_POLLED_NO_DATA,   /* receive: RX FIFO empty                            */
    CAN_POLLED_TIMEOUT,   /* send: SENTDATA not set within 200 ms (request
                             aborted; the frame may still have gone out as
                             the abort took effect), or the TX mailbox was
                             still busy with an earlier abort (nothing sent) */
    CAN_POLLED_BUS_OFF,   /* bus-off detected; driver re-initialized itself —
                             caller must keep waiting / retry                  */
    CAN_POLLED_ERROR,     /* bad argument (bitrate, ID, dlc) or mode-switch
                             failure                                           */
    CAN_POLLED_RECOVERY_FAILED, /* receive/send: bus-off recovery could not
                             bring the controller back; CAN0 is unusable
                             until the caller re-runs can_polled_init()        */
} can_polled_status_t;

/* Bring up CAN0 at 250000 or 500000 bit/s (16 tq/bit, sample point 75 %,
 * SJW 4 tq). Any other bitrate is rejected with CAN_POLLED_ERROR. Acceptance
 * filtering is exact: only frames whose 11-bit standard ID equals
 * rx_filter_id are received (4-deep hardware RX FIFO, mailboxes 28–31). */
can_polled_status_t can_polled_init(uint32_t bitrate_bps, uint32_t rx_filter_id);

/* Non-blocking. Returns CAN_POLLED_NO_DATA when the RX FIFO is empty.
 * On CAN_POLLED_OK: *id = 11-bit standard ID, *dlc = 0..8, data[0..*dlc-1]
 * filled. */
can_polled_status_t can_polled_receive(uint32_t *id, uint8_t *dlc, uint8_t data[8]);

/* Transmit one frame from a single TX mailbox and poll SENTDATA to
 * completion for at most 200 ms. Before MB[0] is written the mailbox is made
 * idle: a request still latched from an earlier send is aborted and its
 * completion flag cleared (at most 2 ms). On the 200 ms bound the request is
 * aborted the same way and CAN_POLLED_TIMEOUT returned. Worst-case blocking:
 * about 204 ms. */
can_polled_status_t can_polled_send(uint32_t id, uint8_t dlc, const uint8_t data[8]);

/* Module-stop CAN0 and restore P102/P103 PFS to reset state (GPIO input).
 * Call before jump-to-app. */
void can_polled_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* CAN_POLLED_H */
