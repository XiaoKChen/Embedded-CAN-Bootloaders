/* =============================================================================
 * can_polled.c — Polling-only CAN0 driver for the Renesas RA4M1 (UNO R4 Minima)
 * =============================================================================
 * Register-level, zero interrupts. Register/bit facts cross-checked against
 * FSP sources vendored under vendor/ra4m1-headers/reference/ and the RA4M1
 * HW manual (R01UM0007) CAN chapter; non-obvious values cite file:line below.
 * ============================================================================= */

#include "can_polled.h"

#include <stdbool.h>
#include <stdint.h>

/* FSP's device header requires IRQn_Type to be defined first; FSP itself
 * includes bsp_exceptions.h before R7FA4M1AB.h (both vendored together in
 * vendor/ra4m1-headers/). */
#include "bsp_exceptions.h"
#include "R7FA4M1AB.h"

/* -----------------------------------------------------------------------------
 * Constants
 * ----------------------------------------------------------------------------- */

/* CAN0 module-stop: MSTPCRB bit 2 (reference/bsp_module_stop.h:221-222,
 * "1U << (2U - channel)" for FSP_IP_CAN channel 0). */
#define CAN0_MSTP_BIT           (1UL << 2)

/* PFS PSEL code for the CAN function is 0x10, PSEL field at bits [28:24]
 * (reference/r_ioport.h:33,389 — IOPORT_PERIPHERAL_CAN = 0x10 << 24).
 * PMR (bit 16) switches the pin to peripheral mode. */
#define PFS_PSEL_CAN            (0x10UL << R_PFS_PORT_PIN_PmnPFS_PSEL_Pos)
#define PFS_PMR                 (1UL << R_PFS_PORT_PIN_PmnPFS_PMR_Pos)

/* PWPR unlock/lock sequences (reference/bsp_io.h:408-451): clear B0WI first,
 * then flip PFSWE; each step is a full-byte write. */
#define PWPR_PFSWE              (1U << 6)
#define PWPR_B0WI               (1U << 7)

/* D4 = P103 = CTX0, D5 = P102 = CRX0 (ArduinoCore-renesas MINIMA pinmux). */
#define CAN_TX_PORT             1
#define CAN_TX_PIN              3
#define CAN_RX_PORT             1
#define CAN_RX_PIN              2

/* CTLR.CANM encoding: 00 = operate, 01 = reset, 10 = halt. STR mirrors the
 * current mode in RSTST/HLTST/SLPST (bits 8-10, mask 0x0700) — mode reached
 * when (STR & 0x0700) == (CANM << 8) (reference/r_can.c:53,1130-1140). */
#define CAN_MODE_OPERATE        0U
#define CAN_MODE_RESET          1U
#define CAN_MODE_HALT           2U
#define CAN_STR_MODE_MASK       0x0700U

/* Bit timing on PCLKB = 24 MHz: 16 tq/bit, SS=1, TSEG1=11, TSEG2=4, SJW=1
 * (sample point 75 %). BCR fields hold value-1 (reference/r_can.c:283-288). */
#define CAN_PCLKB_HZ            24000000UL
#define CAN_BRP_250K            6U
#define CAN_BRP_500K            3U
#define CAN_BCR_SEGMENTS  (((11UL - 1U) << R_CAN0_BCR_TSEG1_Pos) | \
                           ((4UL - 1U)  << R_CAN0_BCR_TSEG2_Pos) | \
                           ((1UL - 1U)  << R_CAN0_BCR_SJW_Pos))
/* BCR.CCLKS = 0 selects PCLKB (reference/r_can.h:126-130). */

/* Standard ID sits at bits [28:18] of MB.ID / MKR / FIDCR. */
#define CAN_SID_MAX             0x7FFUL

/* MCTL bits (device header MCTL_TX/MCTL_RX): TRMREQ=0x80 requests transmit,
 * SENTDATA=0x01 signals completion. SENTDATA and TRMREQ cannot be cleared in
 * one write — write 0x00 twice (reference/r_can.c:1043-1051). */
#define MCTL_TRMREQ             0x80U
#define MCTL_SENTDATA           0x01U

/* RFCR: RFE=bit0 enables the RX FIFO, RFMLF=bit4 message-lost flag,
 * RFEST=bit7 empty flag. Reading MB[28] accesses the FIFO head; writing 0xFF
 * to RFPCR advances the CPU-side pointer (reference/r_can.c:776-781,946-951). */
#define RX_FIFO_MB              28U
#define TX_MAILBOX              0U

/* Mailboxes 0-23 are normal mailboxes in FIFO mode; MKIVLR bits 24-31 must
 * stay 0 in FIFO mode (reference/r_can.c:350-352). */
#define CAN_MKIVLR_FIFO_MASK    0x00FFFFFFUL
#define CAN_NORMAL_MAILBOXES    24U

/* Loop bounds: a mode switch takes a few CAN-clock cycles; a transmit can
 * suffer arbitration loss and error retries. At 48 MHz CPU / 250 kbit/s bus,
 * 2M polls (>= ~5 cycles each) bounds the wait to roughly 200 ms. */
#define CAN_MODE_TIMEOUT_LOOPS  100000UL
#define CAN_TX_TIMEOUT_LOOPS    2000000UL

/* -----------------------------------------------------------------------------
 * File-local state (plain .bss scalars — no static init order dependencies)
 * ----------------------------------------------------------------------------- */
static uint32_t s_bitrate_bps;
static uint32_t s_rx_filter_id;

/* -----------------------------------------------------------------------------
 * Helpers
 * ----------------------------------------------------------------------------- */

/* Clear SLPM, request CANM = mode, poll STR until the mode is reached.
 * HW manual: write CTLR, then confirm via STR RSTST/HLTST flags. */
static can_polled_status_t can_mode_set(uint16_t mode)
{
    uint16_t ctlr = R_CAN0->CTLR;
    ctlr &= (uint16_t) ~(R_CAN0_CTLR_SLPM_Msk | R_CAN0_CTLR_CANM_Msk);
    ctlr |= (uint16_t) (mode << R_CAN0_CTLR_CANM_Pos);
    R_CAN0->CTLR = ctlr;

    for (uint32_t i = 0; i < CAN_MODE_TIMEOUT_LOOPS; i++) {
        if ((R_CAN0->STR & CAN_STR_MODE_MASK) == (uint16_t) (mode << R_CAN0_CTLR_CANM_Pos)) {
            return CAN_POLLED_OK;
        }
    }
    return CAN_POLLED_ERROR;
}

/* Route one pin to CAN0 (or back to reset state when pfs_value == 0).
 * HW manual I/O Ports chapter: set PSEL while PMR = 0, then set PMR —
 * hence the two writes. */
static void can_pin_config(uint32_t port, uint32_t pin, uint32_t pfs_value)
{
    R_PMISC->PWPR = 0;                                   /* clear B0WI       */
    R_PMISC->PWPR = PWPR_PFSWE;                          /* enable PFS write */

    R_PFS->PORT[port].PIN[pin].PmnPFS = pfs_value & ~PFS_PMR;
    R_PFS->PORT[port].PIN[pin].PmnPFS = pfs_value;

    R_PMISC->PWPR = 0;                                   /* clear PFSWE      */
    R_PMISC->PWPR = PWPR_B0WI;                           /* re-lock          */
}

/* Bus-off recovery: full re-init, then report CAN_POLLED_BUS_OFF so the
 * caller keeps waiting (never proceed with a half-programmed slot). */
static can_polled_status_t can_bus_off_recover(void)
{
    (void) can_polled_init(s_bitrate_bps, s_rx_filter_id);
    return CAN_POLLED_BUS_OFF;
}

/* -----------------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------------- */

can_polled_status_t can_polled_init(uint32_t bitrate_bps, uint32_t rx_filter_id)
{
    uint32_t prescaler;

    if (bitrate_bps == 250000UL) {
        prescaler = CAN_BRP_250K;
    } else if (bitrate_bps == 500000UL) {
        prescaler = CAN_BRP_500K;
    } else {
        return CAN_POLLED_ERROR;
    }
    if (rx_filter_id > CAN_SID_MAX) {
        return CAN_POLLED_ERROR;
    }

    /* Release CAN0 module-stop; dummy read-back ensures the write completes
     * before the first CAN register access. */
    R_MSTP->MSTPCRB &= ~CAN0_MSTP_BIT;
    (void) R_MSTP->MSTPCRB;

    can_pin_config(CAN_TX_PORT, CAN_TX_PIN, PFS_PSEL_CAN | PFS_PMR);
    can_pin_config(CAN_RX_PORT, CAN_RX_PIN, PFS_PSEL_CAN | PFS_PMR);

    /* Reset mode: MBM/IDFM/MLM and BCR may only be written here
     * (reference/r_can.c:262-288). Entering reset also clears a latched
     * bus-off condition and the error counters. */
    if (can_mode_set(CAN_MODE_RESET) != CAN_POLLED_OK) {
        return CAN_POLLED_ERROR;
    }

    /* FIFO mailbox mode (MBM=1), standard ID (IDFM=00), overwrite (MLM=0),
     * stay in reset while writing. */
    R_CAN0->CTLR = (uint16_t) (R_CAN0_CTLR_MBM_Msk |
                               (CAN_MODE_RESET << R_CAN0_CTLR_CANM_Pos));

    R_CAN0->BCR = ((prescaler - 1UL) << R_CAN0_BCR_BRP_Pos) | CAN_BCR_SEGMENTS;

    /* Halt mode: mailbox controls, masks and FIFO ID compare registers are
     * configured here (reference/r_can.c:291-373). */
    if (can_mode_set(CAN_MODE_HALT) != CAN_POLLED_OK) {
        return CAN_POLLED_ERROR;
    }

    for (uint32_t i = 0; i < CAN_NORMAL_MAILBOXES; i++) {
        R_CAN0->MCTL_RX[i] = 0;                /* disable, clear stale flags */
    }

    /* Accept ONLY rx_filter_id: both RX-FIFO ID-compare registers hold the
     * same standard ID and both masks compare every SID bit
     * (reference/r_can.c:354-373 — FIDCR[0..1], MKR[6..7] are the FIFO's). */
    R_CAN0->FIDCR[0] = rx_filter_id << R_CAN0_MB_ID_SID_Pos;
    R_CAN0->FIDCR[1] = rx_filter_id << R_CAN0_MB_ID_SID_Pos;
    R_CAN0->MKR[6]   = CAN_SID_MAX << R_CAN0_MB_ID_SID_Pos;
    R_CAN0->MKR[7]   = CAN_SID_MAX << R_CAN0_MB_ID_SID_Pos;
    R_CAN0->MKIVLR   = CAN_MKIVLR_FIFO_MASK;   /* bits 24-31 must stay 0     */

    R_CAN0->MIER = 0;                          /* poll-only: no interrupts   */
    R_CAN0->EIER = 0;

    if (can_mode_set(CAN_MODE_OPERATE) != CAN_POLLED_OK) {
        return CAN_POLLED_ERROR;
    }

    /* Enable RX FIFO after entering operate mode (reference/r_can.c:397-411). */
    R_CAN0->RFCR = R_CAN0_RFCR_RFE_Msk;

    s_bitrate_bps  = bitrate_bps;
    s_rx_filter_id = rx_filter_id;
    return CAN_POLLED_OK;
}

can_polled_status_t can_polled_receive(uint32_t *id, uint8_t *dlc, uint8_t data[8])
{
    if ((id == 0) || (dlc == 0) || (data == 0)) {  /* null pointers */
        return CAN_POLLED_ERROR;
    }
    if (R_CAN0->STR & R_CAN0_STR_BOST_Msk) {
        return can_bus_off_recover();
    }
    if (R_CAN0->RFCR & R_CAN0_RFCR_RFEST_Msk) {
        return CAN_POLLED_NO_DATA;             /* FIFO empty                 */
    }

    /* MB[28] is the RX FIFO access window (reference/r_can.c:944,776-781). */
    *id = (R_CAN0->MB[RX_FIFO_MB].ID >> R_CAN0_MB_ID_SID_Pos) & CAN_SID_MAX;

    uint8_t len = (uint8_t) (R_CAN0->MB[RX_FIFO_MB].DL & 0x0FU);
    if (len > 8U) {
        len = 8U;                              /* DLC > 8 is legal on the bus */
    }
    *dlc = len;
    for (uint8_t i = 0; i < len; i++) {
        data[i] = R_CAN0->MB[RX_FIFO_MB].D[i];
    }

    R_CAN0->RFPCR = 0xFFU;                     /* advance CPU-side pointer   */

    if (R_CAN0->RFCR & R_CAN0_RFCR_RFMLF_Msk) {
        R_CAN0->RFCR = R_CAN0_RFCR_RFE_Msk;    /* clear message-lost, keep RFE */
    }
    return CAN_POLLED_OK;
}

can_polled_status_t can_polled_send(uint32_t id, uint8_t dlc, const uint8_t data[8])
{
    if ((id > CAN_SID_MAX) || (dlc > 8U) || ((data == 0) && (dlc > 0U))) {
        return CAN_POLLED_ERROR;
    }
    if (R_CAN0->STR & R_CAN0_STR_BOST_Msk) {
        return can_bus_off_recover();
    }

    R_CAN0->MB[TX_MAILBOX].ID = id << R_CAN0_MB_ID_SID_Pos;  /* IDE=0, RTR=0 */
    R_CAN0->MB[TX_MAILBOX].DL = dlc;
    for (uint8_t i = 0; i < dlc; i++) {
        R_CAN0->MB[TX_MAILBOX].D[i] = data[i];
    }

    R_CAN0->MCTL_TX[TX_MAILBOX] = MCTL_TRMREQ;

    for (uint32_t i = 0; i < CAN_TX_TIMEOUT_LOOPS; i++) {
        if (R_CAN0->MCTL_TX[TX_MAILBOX] & MCTL_SENTDATA) {
            /* SENTDATA and TRMREQ cannot clear in one write — write 0 twice
             * (reference/r_can.c:1043-1051). */
            R_CAN0->MCTL_TX[TX_MAILBOX] = 0;
            R_CAN0->MCTL_TX[TX_MAILBOX] = 0;
            return CAN_POLLED_OK;
        }
        if (R_CAN0->STR & R_CAN0_STR_BOST_Msk) {
            return can_bus_off_recover();
        }
    }

    R_CAN0->MCTL_TX[TX_MAILBOX] = 0;           /* abort the pending request  */
    R_CAN0->MCTL_TX[TX_MAILBOX] = 0;
    return CAN_POLLED_TIMEOUT;
}

void can_polled_deinit(void)
{
    /* Reset mode halts all bus activity; result deliberately ignored — we
     * are stopping the module regardless. */
    (void) can_mode_set(CAN_MODE_RESET);

    R_MSTP->MSTPCRB |= CAN0_MSTP_BIT;
    (void) R_MSTP->MSTPCRB;

    /* PmnPFS reset value is 0x00000000 (GPIO input, no pull, PSEL clear). */
    can_pin_config(CAN_TX_PORT, CAN_TX_PIN, 0);
    can_pin_config(CAN_RX_PORT, CAN_RX_PIN, 0);
}
