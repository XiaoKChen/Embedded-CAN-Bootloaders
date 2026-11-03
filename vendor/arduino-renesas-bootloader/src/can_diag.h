/**
 * @file    can_diag.h
 * @brief   CAN_DIAG bench instrumentation of the CAN bring-up (closed
 *          investigation, kept for re-use on the bench).
 *
 * Stage-marker frames {0xD1, stage, ...} on the magic-entry (CAN update
 * mode) path, a heartbeat, and the one-boot DIAG4 staircase that found the
 * CAN transceiver's dependency on the USB regulator rail (production fix:
 * can_xcvr_usb_rail_power_on in main.c). Built only with CAN_LOADER and
 * CAN_DIAG=1 (Makefile.minima adds can_diag.c then); in every other build
 * the staircase hooks below are no-ops, so main.c calls them unconditionally.
 *
 * Owns:         The diag marker formats, the staircase run-once flag
 *               (VBTBKR[8..11]) and the latched flash-open error codes.
 * Not here:     The production CAN service, boot decision and flash gate
 *               (main.c), the CAN driver (drivers/can_polled.h).
 * Invariants:   Never shipped; CI does not build it. A diag board never
 *               resets off the bench on a CAN failure, it parks. The
 *               staircase runs at most once per power cycle and ends in one
 *               reset, so the real boot decision starts from clean
 *               peripherals.
 * Depends on:   The FSP BSP and board API, drivers/can_polled.h,
 *               boot/boot_config.h.
 *
 * @see     can_diag.c, main.c, docs/build.md (size gate, diag build)
 */

#ifndef CAN_DIAG_H
#define CAN_DIAG_H

#include <stdbool.h>
#include <stdint.h>

#if defined(CAN_LOADER) && defined(CAN_DIAG)

/* fsp_err_t of R_FLASH_LP_Open / R_FLASH_LP_Reset, latched by main.c's
 * flash_lp_open_df_read_ready for the stage-1 marker. */
extern uint16_t can_diag_open_err;
extern uint16_t can_diag_reset_err;

/* Stage 0: retry-based CAN bring-up on the default IDs before any flash
 * call; latches DFLCTL as found on entry. Returns true when CAN is up. */
bool can_diag_entry_bringup(void);

/* Stages 1-3: flash gate, loaded IDs, and a re-init on cmd_id when it is not
 * the default. Returns whether CAN is up afterwards. */
bool can_diag_entry_report(bool df_readable, uint32_t cmd_id, uint32_t reply_id,
                           bool can_up);

/* Call once per service-loop pass: a heartbeat every ~500 ms of passes. */
void can_diag_heartbeat(uint32_t reply_id);

/* DIAG4 staircase: begin (true = this boot runs it), one probe per preamble
 * step, end (reset, or park if the run-once flag did not stick). */
bool can_diag_stair_begin(void);
void can_diag_stair_probe(uint8_t step);
void can_diag_stair_end(void);

#else

#define can_diag_stair_begin()      (false)
#define can_diag_stair_probe(step)  ((void) (step))
#define can_diag_stair_end()        ((void) 0)

#endif /* CAN_LOADER && CAN_DIAG */

#endif /* CAN_DIAG_H */
