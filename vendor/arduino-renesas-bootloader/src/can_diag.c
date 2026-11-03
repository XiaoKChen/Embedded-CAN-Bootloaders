/**
 * @file    can_diag.c
 * @brief   CAN_DIAG bench instrumentation (see can_diag.h for the contract).
 *
 * Implementation notes: compiled only into CAN_LOADER=1 CAN_DIAG=1 builds
 * of Makefile.minima. The other boards' Makefiles compile every .c file in
 * src and have neither flag nor the repo's src/ on the include path, so the
 * body is guarded by the same condition as can_diag.h. Marker sends ignore
 * their result: a lost diag frame is acceptable, and the update engine stays
 * fully functional alongside.
 *
 * @see     can_diag.h, main.c
 */

#include "can_diag.h"

#if defined(CAN_LOADER) && defined(CAN_DIAG)

#include "bsp_api.h"
#include "bsp/board_api.h"
#include "flash.h"  /* BSP_PRV_PRCR_* */

#include "boot/boot_config.h"
#include "drivers/can_polled.h"

#define CAN_DIAG_MARKER              0xD1u
#define CAN_DIAG_STAGE_CAN_UP        0x00u  /* after default-ID can_polled_init  */
#define CAN_DIAG_STAGE_FLASH_GATE    0x01u  /* after flash_lp_open_df_read_ready */
#define CAN_DIAG_STAGE_IDS_LOADED    0x02u  /* after can_load_ids                */
#define CAN_DIAG_STAGE_REFILTER      0x03u  /* after re-init on the loaded ID    */
#define CAN_DIAG_STAGE_STAIR_RESULT  0xE4u  /* DIAG4 staircase verdict frame     */
#define CAN_DIAG_STAGE_STAIR_LOG     0xE5u  /* DIAG4 failed-attempt register log */
#define CAN_DIAG_STAGE_HEARTBEAT     0xFBu
#define CAN_DIAG_NO_REINIT           0xEEu  /* stage-3 status: IDs = defaults    */
/* Crude ~500 ms heartbeat period by busy loop count: no SysTick guaranteed in
 * this loop, and no interrupts allowed. One empty-FIFO service pass is a
 * handful of register reads at 48 MHz — order-of-magnitude is all we need. */
#define CAN_DIAG_HEARTBEAT_LOOPS     500000u

/* DIAG3 retry-bisect knobs. One attempt is bounded by the driver's ~200 ms
 * SENTDATA wait (twice, with the second-send probe) plus the 100 ms settle,
 * so the 3 s budget dominates when sends time out slowly and the attempt cap
 * when they fail fast; both together keep the loop finite even if SysTick
 * (board_millis) were dead. */
#define CAN_DIAG_RETRY_MAX_ATTEMPTS  30u
#define CAN_DIAG_RETRY_BUDGET_MS     3000u
#define CAN_DIAG_RETRY_SETTLE_MS     100u
#define CAN_DIAG_SECOND_SEND         0x80u  /* b2 flag: 2nd TX of the attempt */

uint16_t can_diag_open_err;   /* fsp_err_t of R_FLASH_LP_Open  */
uint16_t can_diag_reset_err;  /* fsp_err_t of R_FLASH_LP_Reset */
static uint8_t can_diag_dflctl_on_entry;

static void can_diag_send(uint32_t id, uint8_t stage, uint8_t b2, uint8_t b3,
                          uint8_t b4, uint8_t b5, uint8_t b6, uint8_t b7)
{
  const uint8_t frame[8] = { CAN_DIAG_MARKER, stage, b2, b3, b4, b5, b6, b7 };
  (void) can_polled_send(id, 8, frame);  /* diag marker: loss is acceptable */
}

/* ---- DIAG3: retry-based bring-up bisect ----------------------------------
 * DIAG2 established that one init+send right after board_init() NEVER
 * transmits while the identical calls later in the DFU path do. One attempt
 * here = can_polled_init -> marker send -> (on failure) ONE more send on the
 * SAME init -> deinit + 100 ms settle -> retry, until a frame is ACKed on
 * the wire or the budget runs out. The first frame that arrives carries the
 * whole story:
 *   b2  attempt number (1-based); bit7 SET = the frame went out on the
 *       SECOND send of its attempt — init was fine and only the first TX
 *       after bring-up is lost (first-TX quirk, not settle time)
 *   b3  init status of the winning attempt
 *   b4  previous attempt's last send status (0xFF = none, first attempt)
 *   b5  RSTSR1 low byte (bit2 SWRF = software-reset entry), b6 RSTSR2
 *   b7  previous attempt's TECR, captured right after its failed send and
 *       before deinit: 0 = the controller never drove the bus (mode/sync
 *       problem); >= 8 and climbing toward 128+ = it DID transmit but every
 *       frame died on the wire (bit-timing/ACK problem)
 * On total failure the last attempt's init is deliberately left in place
 * (module clocked, registers safe to poll) and *out_sent stays false. */
static can_polled_status_t can_diag_bringup_retry(uint8_t stage, bool *out_sent)
{
  uint8_t rstsr1_lo = (uint8_t) R_SYSTEM->RSTSR1;
  uint8_t rstsr2    = (uint8_t) R_SYSTEM->RSTSR2;
  uint8_t prev_send = 0xFFu;                       /* no previous attempt */
  uint8_t prev_tecr = 0;
  can_polled_status_t init_status = CAN_POLLED_ERROR;
  uint32_t start_ms = board_millis();

  *out_sent = false;
  for (uint8_t attempt = 1; ; attempt++) {
    init_status = can_polled_init(CAN_BITRATE_BPS, CAN_ID_HOST_TO_BOOT);

    uint8_t frame[8] = { CAN_DIAG_MARKER, stage, attempt, (uint8_t) init_status,
                         prev_send, rstsr1_lo, rstsr2, prev_tecr };
    can_polled_status_t send_status = init_status;
    if (init_status == CAN_POLLED_OK) {
      send_status = can_polled_send(CAN_ID_BOOT_TO_HOST, 8, frame);
      if (send_status != CAN_POLLED_OK) {
        /* Priming probe: retry on the SAME init, no delay, no reinit. */
        frame[2] = (uint8_t) (attempt | CAN_DIAG_SECOND_SEND);
        send_status = can_polled_send(CAN_ID_BOOT_TO_HOST, 8, frame);
      }
    }
    if (send_status == CAN_POLLED_OK) {
      *out_sent = true;
      return init_status;
    }
    if ((attempt >= CAN_DIAG_RETRY_MAX_ATTEMPTS) ||
        ((board_millis() - start_ms) >= CAN_DIAG_RETRY_BUDGET_MS)) {
      return init_status;  /* leave the last init up: register polls stay safe */
    }
    prev_send = (uint8_t) send_status;
    prev_tecr = R_CAN0->TECR;      /* before deinit's reset clears the counter */
    can_polled_deinit();
    R_BSP_SoftwareDelay(CAN_DIAG_RETRY_SETTLE_MS, BSP_DELAY_UNITS_MILLISECONDS);
  }
}

bool can_diag_entry_bringup(void)
{
  /* The winning frame's attempt count quantifies the settle time this path
   * needs; if every attempt fails the last init stays up. */
  bool stage0_sent;
  can_polled_status_t init_status = can_diag_bringup_retry(CAN_DIAG_STAGE_CAN_UP,
                                                           &stage0_sent);
  (void) stage0_sent;  /* the bench reads success off the wire, not this flag */

  /* Stage 1's byte 7: DFLCTL before main.c's flash open — tests the "DFLEN
   * survived the software reset" hypothesis of the data-flash gate. */
  can_diag_dflctl_on_entry = (uint8_t) R_FACI_LP->DFLCTL;
  return init_status == CAN_POLLED_OK;
}

bool can_diag_entry_report(bool df_readable, uint32_t cmd_id, uint32_t reply_id,
                           bool can_up)
{
  can_diag_send(CAN_ID_BOOT_TO_HOST, CAN_DIAG_STAGE_FLASH_GATE,
                df_readable ? 1u : 0u,
                (uint8_t) can_diag_open_err, (uint8_t) (can_diag_open_err >> 8),
                (uint8_t) can_diag_reset_err, (uint8_t) (can_diag_reset_err >> 8),
                can_diag_dflctl_on_entry);

  can_diag_send(CAN_ID_BOOT_TO_HOST, CAN_DIAG_STAGE_IDS_LOADED,
                df_readable ? 1u : 0u,
                (uint8_t) cmd_id, (uint8_t) (cmd_id >> 8),
                (uint8_t) reply_id, (uint8_t) (reply_id >> 8), 0);

  /* Stage 3: re-filter onto the loaded cmd ID when it differs from the
   * default; the marker goes out on the LOADED reply ID so the bench proves
   * that ID actually transmits. */
  uint8_t refilter_status = CAN_DIAG_NO_REINIT;
  if (cmd_id != CAN_ID_HOST_TO_BOOT) {
    can_polled_deinit();
    can_polled_status_t init_status = can_polled_init(CAN_BITRATE_BPS, cmd_id);
    can_up = (init_status == CAN_POLLED_OK);
    refilter_status = (uint8_t) init_status;
  }
  can_diag_send(reply_id, CAN_DIAG_STAGE_REFILTER, refilter_status, 0, 0, 0, 0, 0);
  return can_up;
}

void can_diag_heartbeat(uint32_t reply_id)
{
  static uint32_t hb_loops;
  static uint8_t hb_seq;

  if (++hb_loops < CAN_DIAG_HEARTBEAT_LOOPS) {
    return;
  }
  hb_loops = 0;
  can_diag_send(reply_id, CAN_DIAG_STAGE_HEARTBEAT, hb_seq, 0, 0, 0, 0, 0);
  /* Every 4th heartbeat also on the default reply ID, so the board is
   * provably alive no matter which reply ID the bench watches. */
  if (((hb_seq & 3u) == 0u) && (reply_id != CAN_ID_BOOT_TO_HOST)) {
    can_diag_send(CAN_ID_BOOT_TO_HOST, CAN_DIAG_STAGE_HEARTBEAT, hb_seq, 0, 0, 0, 0, 0);
  }
  hb_seq++;
}

/* ---- DIAG4: one-boot self-bisecting staircase -----------------------------
 * DIAG3 (hardware-confirmed via SWD readback) ruled timing out: 30 bare
 * init+send attempts over 3 s right after board_init() never put a frame on
 * a live, ACKing bus — while the IDENTICAL driver calls after the DFU
 * preamble (IOPORT open -> GPT open/start -> FLASH_LP open/reset ->
 * tud_init) always transmit. So at least one preamble piece is a hard
 * functional prerequisite for CAN0 TX. The staircase finds WHICH in one
 * boot. Bench verdict (2026-07, real bus): step 4 (+tud_init) won on its
 * first attempt after 12 failures across steps 0-3, every one TX-timeout
 * with TEC = 0 in operate mode -> the USB LDO (USBMC.VDCEN) powers the
 * external CAN transceiver rail; production fix in
 * can_xcvr_usb_rail_power_on. The staircase does NOT duplicate the preamble
 * calls (a first cut did, and
 * LTO's constprop clones of tud_init/R_GPT_Open/R_GPT_PeriodSet cost 636 B
 * the 16 KiB region does not have). Instead, the staircase boot jumps into
 * the REAL `bootloader:` path in main.c with a can_diag_stair_probe()
 * interleaved after each preamble piece, gated on can_diag_stair_active, and
 * ends in a reset before the DFU loop (can_diag_stair_end). Fidelity is exact by construction: the pieces
 * that run are the production calls themselves, in production order.
 *
 * Each probe: can_polled_init on the compiled default IDs, then up to
 * CAN_DIAG_STAIR_SEND_TRIES sends. Every attempt's payload IS the verdict
 * frame, so the first frame on the bus names the winner:
 *   {0xD1, 0xE4, step, attempt, STR lo, STR hi, TECR, ECSR}
 *   step: 0 = bare (would contradict DIAG3), 1 = +IOPORT, 2 = +GPT,
 *         3 = +FLASH_LP open/reset, 4 = +tud_init, 0xFF = nothing worked
 *   (registers sampled after init, just before that attempt's TRMREQ).
 * The success latch stops later probes, so exactly one step ever reports.
 *
 * Every FAILED attempt records {STR, ECSR, TECR} into a one-entry "last
 * failure before success" snapshot, emitted right after the verdict:
 *   {0xD1, 0xE5, idx, STR lo, STR hi, ECSR, TECR, kind}
 *   idx = number of failed attempts preceding the success (0 = none -> no
 *   E5 frame). idx maps back to (step, attempt) because attempts run in
 *   fixed step order, SEND_TRIES per step. STR RSTST/HLTST/SLPST (bits
 *   8/9/10) = mode the controller was REALLY in; TECR = 0 -> it never
 *   drove the bus (mode/clock problem), climbing -> frames died on the
 *   wire; ECSR error-code flags (ADEF/ACKEF/SEF/FEF/AEF/BE0F/BE1F) say HOW
 *   they died — ACKEF = nobody ACKed, stuff/bit errors = bit-timing wrong.
 *   kind: 0xA5 = TX timed out (registers sampled after the driver's abort;
 *   MCTL0 latch evidence not captured), 0xEE = can_polled_init failed.
 * Additionally, attempts 2..SEND_TRIES of any step carry post-failure
 * TECR/ECSR/STR in their OWN E4 payload (same init, sampled pre-send), so
 * a step that wins on attempt >= 2 documents its own failures.
 * (DIAG4 size cuts, in order applied: [1] the spec'd 8-entry per-attempt
 * register log -> one-entry last-failure snapshot; [2] the raw-MCTL TX
 * probe (captured MCTL[0] before the driver's abort-clear) -> driver sends,
 * losing the "did TRMREQ/SENTDATA latch" byte. Both forced by the 16 KiB
 * region: full design measured 16,604 B, snapshot-only 16,472 B, this
 * layout fits. The 0xEE/0xA5 kind byte preserves the E5 frame format.)
 *
 * Run-once / cannot-boot-loop design: VBTBKR[8..11] (VBATT backup domain,
 * the proven mechanism behind the double-tap and CAN-update magics) holds a
 * "staircase done" word. Garbage on the first boot after power-up -> run
 * the staircase, but write the flag FIRST so even a crash mid-staircase
 * cannot re-enter it, then end in one NVIC_SystemReset so the app/DFU/CAN
 * boot decisions always start from clean peripherals (GPT/USB/flash state
 * from the staircase never leaks into the app). If the flag readback ever
 * failed to stick, the board parks in the normal DFU+CAN service loop
 * instead of resetting — degraded but bounded: no reset loop, no brick.
 * Later boots skip every probe until the next power cycle. A pending
 * double-tap or CAN-update magic is left untouched by the staircase boot
 * and honored one boot (~1 s) later. */
#define CAN_DIAG_STAIR_DONE_DATA   (*((volatile uint32_t *) (uintptr_t) &R_SYSTEM->VBTBKR[8]))
#define CAN_DIAG_STAIR_DONE_MAGIC  0x5D1A6435u
#define CAN_DIAG_STAIR_SEND_TRIES  3u
#define CAN_DIAG_STAIR_STEP_NONE   0xFFu
#define CAN_DIAG_STAIR_TX_FAILED   0xA5u    /* kind: send timed out          */
#define CAN_DIAG_STAIR_INIT_FAILED 0xEEu    /* kind: init failed, no TX done */

static bool can_diag_stair_active;          /* this boot is the staircase boot */
static bool can_diag_stair_won;             /* success latch: stop later probes */

/* One struct, one base address: five separate statics cost five literal-pool
 * entries. */
static struct {
  uint16_t str;
  uint8_t  ecsr;
  uint8_t  tecr;
  uint8_t  kind;
  uint8_t  count;
} can_diag_stair_fail;

static void can_diag_stair_log_add(uint8_t kind)
{
  can_diag_stair_fail.count++;
  can_diag_stair_fail.str  = R_CAN0->STR;
  can_diag_stair_fail.ecsr = R_CAN0->ECSR;
  can_diag_stair_fail.tecr = R_CAN0->TECR;
  can_diag_stair_fail.kind = kind;
}

/* One staircase level: bring CAN up, try to get the verdict frame for
 * `step` onto the bus, tear CAN down again (the next preamble piece must
 * run with CAN down, exactly like the production path). No-op on non-
 * staircase boots and after a step has already won. */
void can_diag_stair_probe(uint8_t step)
{
  if (!can_diag_stair_active || can_diag_stair_won) {
    return;
  }
  if (can_polled_init(CAN_BITRATE_BPS, CAN_ID_HOST_TO_BOOT) != CAN_POLLED_OK) {
    can_diag_stair_log_add(CAN_DIAG_STAIR_INIT_FAILED);
    can_polled_deinit();
    return;
  }
  for (uint8_t attempt = 1; attempt <= CAN_DIAG_STAIR_SEND_TRIES; attempt++) {
    /* The attempt's payload IS the E4 verdict frame; registers sampled just
     * before the send, so attempts >= 2 carry the previous failure's TECR/
     * ECSR (same init — can_polled_send's abort clears no error state). */
    uint16_t str = R_CAN0->STR;
    const uint8_t frame[8] = { CAN_DIAG_MARKER, CAN_DIAG_STAGE_STAIR_RESULT,
                               step, attempt, (uint8_t) str, (uint8_t) (str >> 8),
                               R_CAN0->TECR, R_CAN0->ECSR };
    if (can_polled_send(CAN_ID_BOOT_TO_HOST, 8, frame) == CAN_POLLED_OK) {
      if (can_diag_stair_fail.count != 0) {
        can_diag_send(CAN_ID_BOOT_TO_HOST, CAN_DIAG_STAGE_STAIR_LOG,
                      can_diag_stair_fail.count,
                      (uint8_t) can_diag_stair_fail.str,
                      (uint8_t) (can_diag_stair_fail.str >> 8),
                      can_diag_stair_fail.ecsr, can_diag_stair_fail.tecr,
                      can_diag_stair_fail.kind);
      }
      can_diag_stair_won = true;
      break;
    }
    can_diag_stair_log_add(CAN_DIAG_STAIR_TX_FAILED);
  }
  can_polled_deinit();
}

static void can_diag_stair_set_done(void)
{
  R_SYSTEM->PRCR = (uint16_t) BSP_PRV_PRCR_PRC1_UNLOCK;
  CAN_DIAG_STAIR_DONE_DATA = CAN_DIAG_STAIR_DONE_MAGIC;
  R_SYSTEM->PRCR = (uint16_t) BSP_PRV_PRCR_LOCK;
}

/* Runs once per power cycle, from the exact spot DIAG3 confirmed bare CAN TX
 * dead (right after board_init). The only new instruction before probe 0 is
 * the VBTBKR[8] flag read — a VBATT read cannot be the E0 killer, since E0
 * failed with no VBTBKR access at all. A pending double-tap or CAN-update
 * magic survives the staircase boot untouched and is honored one boot later. */
bool can_diag_stair_begin(void)
{
  if (CAN_DIAG_STAIR_DONE_DATA == CAN_DIAG_STAIR_DONE_MAGIC) {
    return false;
  }
  can_diag_stair_set_done();  /* BEFORE the run: a crash cannot re-enter */
  can_diag_stair_active = true;
  return true;
}

void can_diag_stair_end(void)
{
  if (!can_diag_stair_active) {
    return;
  }
  /* Nothing worked? One last round (step 0xFF) so the verdict + snapshot get
   * a chance on the fully-brought-up board; a no-op if a step won. */
  can_diag_stair_probe(CAN_DIAG_STAIR_STEP_NONE);
  if (CAN_DIAG_STAIR_DONE_DATA == CAN_DIAG_STAIR_DONE_MAGIC) {
    NVIC_SystemReset();  /* clean re-entry for the real boot decision */
  }
  /* Flag write did not stick (never observed: the double-tap word proves
   * VBTBKR works). Resetting would loop forever — park in the normal DFU+CAN
   * service loop instead. */
  can_diag_stair_active = false;
}

#endif /* CAN_LOADER && CAN_DIAG */
