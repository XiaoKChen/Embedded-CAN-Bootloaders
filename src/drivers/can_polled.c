/**
 * @file    can_polled.c
 * @brief   Polling-only CAN0 driver for the Renesas RA4M1 (see can_polled.h
 *          for the contract).
 *
 * Implementation notes: register-level, zero interrupts. Register/bit facts
 * cross-checked against FSP sources vendored under
 * vendor/ra4m1-headers/reference/ and the RA4M1 HW manual (R01UM0007) CAN
 * chapter; non-obvious values cite file:line below.
 *
 * @see     can_polled.h
 */

#include "can_polled.h"

#include <stdbool.h>
#include <stdint.h>

/* FSP's device header requires IRQn_Type to be defined first; FSP itself
 * includes bsp_exceptions.h before R7FA4M1AB.h (both vendored together in
 * vendor/ra4m1-headers/). */
#include "bsp_exceptions.h"
#include "R7FA4M1AB.h"

#include "../boot/memory_map.h"  /* BOOT_LOW_FLASH */

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

/* Bit timing on PCLKB = 24 MHz: 16 tq/bit, SS=1, TSEG1=11, TSEG2=4, SJW=4
 * (sample point 75 %; oscillator tolerance in can_polled.h). BCR fields hold
 * value-1 (reference/r_can.c:283-288); the hardware requires
 * TSEG1 > TSEG2 >= SJW (reference/r_can.c:195-206). */
#define CAN_PCLKB_HZ            24000000UL
#define CAN_TQ_PER_BIT          16UL
#define CAN_TSEG1_TQ            11UL
#define CAN_TSEG2_TQ            4UL
#define CAN_SJW_TQ              4UL
#define CAN_BRP(bitrate_bps)    (CAN_PCLKB_HZ / (CAN_TQ_PER_BIT * (bitrate_bps)))
#define CAN_BRP_250K            CAN_BRP(250000UL)
#define CAN_BRP_500K            CAN_BRP(500000UL)
#define CAN_BCR_SEGMENTS  (((CAN_TSEG1_TQ - 1U) << R_CAN0_BCR_TSEG1_Pos) | \
                           ((CAN_TSEG2_TQ - 1U) << R_CAN0_BCR_TSEG2_Pos) | \
                           ((CAN_SJW_TQ - 1U)   << R_CAN0_BCR_SJW_Pos))
/* BCR.CCLKS = 0 selects PCLKB (reference/r_can.h:126-130). */

_Static_assert(1UL + CAN_TSEG1_TQ + CAN_TSEG2_TQ == CAN_TQ_PER_BIT,
               "SS + TSEG1 + TSEG2 must fill the bit");
_Static_assert((CAN_TSEG1_TQ > CAN_TSEG2_TQ) && (CAN_TSEG2_TQ >= CAN_SJW_TQ) &&
               (CAN_TSEG1_TQ <= 16UL) && (CAN_TSEG2_TQ <= 8UL) && (CAN_SJW_TQ <= 4UL),
               "BCR segment fields out of range");
_Static_assert((CAN_PCLKB_HZ % (CAN_TQ_PER_BIT * 250000UL) == 0UL) &&
               (CAN_PCLKB_HZ % (CAN_TQ_PER_BIT * 500000UL) == 0UL),
               "PCLKB must divide exactly into 250k and 500k bit/s");
_Static_assert((CAN_BRP_250K >= 1UL) && (CAN_BRP_250K <= 1024UL) &&
               (CAN_BRP_500K >= 1UL) && (CAN_BRP_500K <= 1024UL),
               "BRP out of the 1..1024 range (reference/r_can.c:30-31)");

/* Standard ID sits at bits [28:18] of MB.ID / MKR / FIDCR. */
#define CAN_SID_MAX             0x7FFUL

/* MCTL bits (device header MCTL_TX/MCTL_RX): TRMREQ=0x80 requests transmit,
 * SENTDATA=0x01 signals completion. SENTDATA and TRMREQ cannot be cleared in
 * one write — write 0x00 twice (reference/r_can.c:1043-1051). */
#define MCTL_TRMREQ             0x80U
#define MCTL_SENTDATA           0x01U
/* TRMABT=0x04 latches a completed transmit abort (device header
 * R_CAN0_MCTL_TX_TRMABT_Msk). */
#define MCTL_TRMABT             0x04U

/* RFCR: RFE=bit0 enables the RX FIFO, RFMLF=bit4 message-lost flag,
 * RFEST=bit7 empty flag. Reading MB[28] accesses the FIFO head; writing 0xFF
 * to RFPCR advances the CPU-side pointer (reference/r_can.c:776-781,946-951). */
#define RX_FIFO_MB              28U
#define TX_MAILBOX              0U

/* Mailboxes 0-23 are normal mailboxes in FIFO mode; MKIVLR bits 24-31 must
 * stay 0 in FIFO mode (reference/r_can.c:350-352). */
#define CAN_MKIVLR_FIFO_MASK    0x00FFFFFFUL
#define CAN_NORMAL_MAILBOXES    24U

/* Wait bounds in real time, measured on the DWT cycle counter at ICLK
 * (48 MHz: HOCO, twice PCLKB — see can_polled.h). A slower ICLK only
 * lengthens them.
 * - Mode switch: reset and halt are entered once the frame in progress ends
 *   (one stuffed 8-byte frame plus an error frame is ~160 bits, 640 us at
 *   250 kbit/s); 10 ms covers 15 of them.
 * - Transmit: arbitration losses and error retries; 200 ms.
 * - Transmit abort: completes when the frame in progress ends (same ~640 us
 *   worst case); 2 ms covers three.
 * - Bus-off: the automatic recovery needs 128 x 11 recessive bits, which
 *   even a saturated bus yields within 128 frame times (~82 ms at
 *   250 kbit/s); a bus-off that outlasts 1 s means the recovery is not
 *   happening. */
#define CAN_CPU_HZ              48000000UL
#define CAN_CYCLES_PER_US       (CAN_CPU_HZ / 1000000UL)
#define CAN_MODE_TIMEOUT_US     10000UL
#define CAN_TX_TIMEOUT_US       200000UL
#define CAN_TX_ABORT_TIMEOUT_US 2000UL
#define CAN_BUS_OFF_REINIT_US   1000000UL

/* -----------------------------------------------------------------------------
 * Types
 * ----------------------------------------------------------------------------- */

/* A bounded wait. `polls` is the fallback for a cycle counter that does not
 * run (no debug block, or stopped): each poll costs at least one cycle, so
 * the poll cap never ends a wait before `cycles` have elapsed, and without
 * the counter it still ends after `cycles` polls. */
typedef struct {
    uint32_t start;
    uint32_t cycles;
    uint32_t polls;
} can_deadline_t;

/* -----------------------------------------------------------------------------
 * File-local state (one plain .bss struct — no static init order
 * dependencies; one base address keeps the BOOT_LOW_FLASH init within the
 * 832 B region)
 * ----------------------------------------------------------------------------- */
static struct {
    uint32_t       bitrate_bps;
    uint32_t       rx_filter_id;
    can_deadline_t bus_off_deadline;  /* forced re-init deadline of the episode */
    bool           bus_off_seen;      /* a bus-off episode is being timed       */
    bool           recovery_failed;   /* sticky until can_polled_init()         */
} s_can;

/* -----------------------------------------------------------------------------
 * Helpers
 * ----------------------------------------------------------------------------- */

/* Enabling the counter is idempotent, so every wait does it and no call
 * order (deinit before init) can leave a wait on a stopped counter. The
 * counter is left running for the application. noinline keeps the
 * BOOT_LOW_FLASH callers inside their 832 B region. */
static __attribute__((noinline)) can_deadline_t can_deadline_start(uint32_t timeout_us)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    can_deadline_t deadline = { DWT->CYCCNT, timeout_us * CAN_CYCLES_PER_US, 0U };
    return deadline;
}

static bool can_deadline_expired(can_deadline_t *deadline)
{
    deadline->polls++;
    return ((DWT->CYCCNT - deadline->start) >= deadline->cycles) ||
           (deadline->polls >= deadline->cycles);
}

/* Clear SLPM, request CANM = mode, poll STR until the mode is reached.
 * HW manual: write CTLR, then confirm via STR RSTST/HLTST flags. */
static can_polled_status_t can_mode_set(uint16_t mode)
{
    uint16_t ctlr = R_CAN0->CTLR;
    ctlr &= (uint16_t) ~(R_CAN0_CTLR_SLPM_Msk | R_CAN0_CTLR_CANM_Msk);
    ctlr |= (uint16_t) (mode << R_CAN0_CTLR_CANM_Pos);
    R_CAN0->CTLR = ctlr;

    can_deadline_t deadline = can_deadline_start(CAN_MODE_TIMEOUT_US);
    do {
        if ((R_CAN0->STR & CAN_STR_MODE_MASK) == (uint16_t) (mode << R_CAN0_CTLR_CANM_Pos)) {
            return CAN_POLLED_OK;
        }
    } while (!can_deadline_expired(&deadline));
    return CAN_POLLED_ERROR;
}

/* Bring the TX mailbox to MCTL_TX = 0, the only state in which MB[0] may be
 * rewritten. Writing 0 clears TRMREQ, which requests an abort of a pending
 * transmit; the abort ends in TRMABT (not sent) or SENTDATA (the frame in
 * progress went out), and only then is that flag cleared, by another write
 * of 0, because TRMREQ and a completion flag cannot clear in one write
 * (reference/r_can.c:1043-1051). Nothing is written while the abort runs.
 * Returns false if the mailbox is still busy after the abort bound. */
static bool can_tx_mailbox_release(void)
{
    can_deadline_t deadline = can_deadline_start(CAN_TX_ABORT_TIMEOUT_US);
    do {
        uint8_t mctl = R_CAN0->MCTL_TX[TX_MAILBOX];
        if (mctl == 0U) {
            return true;
        }
        if (mctl & (MCTL_TRMREQ | MCTL_TRMABT | MCTL_SENTDATA)) {
            R_CAN0->MCTL_TX[TX_MAILBOX] = 0;
        }
    } while (!can_deadline_expired(&deadline));
    return false;
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

/* Bus-off gate for receive/send. CTLR.BOM = 00 makes the controller leave
 * bus-off by itself as ISO 11898-1 requires (128 x 11 recessive bits), so a
 * faulty node cannot flood the bus with error frames; meanwhile this reports
 * CAN_POLLED_BUS_OFF. Only a bus-off that outlasts CAN_BUS_OFF_REINIT_US
 * forces a re-init, so forced recoveries happen at most once per that
 * period; a failed one latches CAN_POLLED_RECOVERY_FAILED. */
static can_polled_status_t can_bus_state(void)
{
    if (s_can.recovery_failed) {
        return CAN_POLLED_RECOVERY_FAILED;
    }
    if ((R_CAN0->STR & R_CAN0_STR_BOST_Msk) == 0U) {
        s_can.bus_off_seen = false;
        return CAN_POLLED_OK;
    }
    if (!s_can.bus_off_seen) {
        s_can.bus_off_seen     = true;
        s_can.bus_off_deadline = can_deadline_start(CAN_BUS_OFF_REINIT_US);
    } else if (can_deadline_expired(&s_can.bus_off_deadline)) {
        if (can_polled_init(s_can.bitrate_bps, s_can.rx_filter_id) != CAN_POLLED_OK) {
            s_can.recovery_failed = true;
            return CAN_POLLED_RECOVERY_FAILED;
        }
    }
    return CAN_POLLED_BUS_OFF;
}

/* -----------------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------------- */

BOOT_LOW_FLASH can_polled_status_t can_polled_init(uint32_t bitrate_bps, uint32_t rx_filter_id)
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
     * ISO 11898-1 automatic bus-off recovery (BOM=00, as FSP's default,
     * reference/r_can.c:272), stay in reset while writing. */
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

    s_can.bitrate_bps     = bitrate_bps;
    s_can.rx_filter_id    = rx_filter_id;
    s_can.bus_off_seen    = false;
    s_can.recovery_failed = false;
    return CAN_POLLED_OK;
}

can_polled_status_t can_polled_receive(uint32_t *id, uint8_t *dlc, uint8_t data[8])
{
    if ((id == 0) || (dlc == 0) || (data == 0)) {  /* null pointers */
        return CAN_POLLED_ERROR;
    }
    can_polled_status_t bus = can_bus_state();
    if (bus != CAN_POLLED_OK) {
        return bus;
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

BOOT_LOW_FLASH can_polled_status_t can_polled_send(uint32_t id, uint8_t dlc, const uint8_t data[8])
{
    if ((id > CAN_SID_MAX) || (dlc > 8U) || ((data == 0) && (dlc > 0U))) {
        return CAN_POLLED_ERROR;
    }
    can_polled_status_t bus = can_bus_state();
    if (bus != CAN_POLLED_OK) {
        return bus;
    }
    if (!can_tx_mailbox_release()) {
        return CAN_POLLED_TIMEOUT;             /* an earlier abort still runs */
    }

    R_CAN0->MB[TX_MAILBOX].ID = id << R_CAN0_MB_ID_SID_Pos;  /* IDE=0, RTR=0 */
    R_CAN0->MB[TX_MAILBOX].DL = dlc;
    for (uint8_t i = 0; i < dlc; i++) {
        R_CAN0->MB[TX_MAILBOX].D[i] = data[i];
    }

    R_CAN0->MCTL_TX[TX_MAILBOX] = MCTL_TRMREQ;

    can_deadline_t deadline = can_deadline_start(CAN_TX_TIMEOUT_US);
    do {
        if (R_CAN0->MCTL_TX[TX_MAILBOX] & MCTL_SENTDATA) {
            /* SENTDATA and TRMREQ cannot clear in one write — write 0 twice
             * (reference/r_can.c:1043-1051). */
            R_CAN0->MCTL_TX[TX_MAILBOX] = 0;
            R_CAN0->MCTL_TX[TX_MAILBOX] = 0;
            return CAN_POLLED_OK;
        }
        if (R_CAN0->STR & R_CAN0_STR_BOST_Msk) {
            /* Request the abort so the frame is not sent stale after the
             * automatic recovery; the next send's release completes it. The
             * gate passed on entry, so this call only starts the episode
             * timer. */
            R_CAN0->MCTL_TX[TX_MAILBOX] = 0;
            (void) can_bus_state();
            return CAN_POLLED_BUS_OFF;
        }
    } while (!can_deadline_expired(&deadline));

    /* Abort and wait it out. If the abort outlasts its bound, the next send's
     * release finishes it before MB[0] is touched. */
    (void) can_tx_mailbox_release();
    return CAN_POLLED_TIMEOUT;
}

BOOT_LOW_FLASH void can_polled_deinit(void)
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
