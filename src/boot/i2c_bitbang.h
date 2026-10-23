/**
 * @file    i2c_bitbang.h
 * @brief   Bounded, best-effort bit-banged I2C master for the board-profile safe writes.
 *
 * The 2nd-stage bootloader and the factory updater force a board's output chip safe (e.g. the
 * SOLO ODU's EMC2305 fan controller to 0 % duty) with a few single-register writes taken from
 * the cancfg board profile. Neither may use the IDF I2C driver (the bootloader has no driver,
 * heap or RTOS), so this module clocks the bus through five pin callbacks: a UM10204 §3.1.16 bus
 * clear (as Embedded-PlatformIO interface-i2c-bus does), then START, addr+W, reg, value, STOP
 * per write at ~100 kHz.
 *
 * Owns:         The bus-clear and write sequences, their timing and their bounds.
 * Not here:     Driving real pins (the caller's i2c_bb_hal_t: boot.c, node_updater/main/main.c);
 *               which writes to make (board_profile.h).
 * Invariants:   Lines are open-drain: a callback releases a line (pulled up) or pulls it LOW,
 *               never drives it HIGH. Both lines are released when any call returns. Every wait
 *               is bounded: all clock stretching within one public call shares a budget of
 *               I2C_BB_STRETCH_BUDGET_US (exhausting it aborts), and i2c_bb_apply_safe_writes()
 *               stops at the first bus fault, so one call delays at most I2C_BB_WORST_CASE_US
 *               plus the callbacks' own overhead.
 * Concurrency:  Single caller; no interrupts or shared state. The caller owns both pins for the
 *               duration of the call.
 * Errors:       i2c_bb_status_t per transaction; i2c_bb_apply_safe_writes() returns the number
 *               of ACKed writes and never fails the caller's boot.
 * Depends on:   board_profile.h (the write list); pure C otherwise, host-tested through a fake HAL.
 *
 * @see     i2c_bitbang.c, tests/test_i2c_bitbang.c, docs/reference/board-profile.md
 */

#ifndef I2C_BITBANG_H
#define I2C_BITBANG_H

#include <stdbool.h>
#include <stdint.h>

#include "board_profile.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -----------------------------------------------------------------------------
 * Timing and bounds
 * ----------------------------------------------------------------------------- */
#define I2C_BB_HALF_PERIOD_US      5u    /* ~100 kHz SCL                               */
#define I2C_BB_MAX_CLEAR_PULSES    9u    /* eight data clocks and an ACK (UM10204)     */
#define I2C_BB_STRETCH_BUDGET_US   2000u /* total SCL-low hold allowed per public call */

/* Half periods per phase, for the bound below. */
#define I2C_BB_CLEAR_HALVES   (1u + 2u * I2C_BB_MAX_CLEAR_PULSES + 4u)  /* settle, 9 pulses, STOP */
#define I2C_BB_WRITE_HALVES   (2u + 3u * 9u * 2u + 3u)  /* idle check + START, 3 bytes + ACK, STOP */

/* Delay budget of one i2c_bb_apply_safe_writes() call: the bus clear, 8 writes and the whole
 * stretch budget. (23 + 8 * 59) * 5 + 2000 = 4,475 us; callback overhead comes on top. */
#define I2C_BB_WORST_CASE_US \
    ((I2C_BB_CLEAR_HALVES + BOARD_PROFILE_I2C_MAX_WRITES * I2C_BB_WRITE_HALVES) * \
         I2C_BB_HALF_PERIOD_US + I2C_BB_STRETCH_BUDGET_US)

/* -----------------------------------------------------------------------------
 * Pin HAL
 * ----------------------------------------------------------------------------- */
typedef struct {
    void *ctx;
    void (*sda_release)(void *ctx, bool release);  /* true = release (high), false = pull LOW */
    void (*scl_release)(void *ctx, bool release);
    bool (*sda_read)(void *ctx);                  /* the pad level                           */
    bool (*scl_read)(void *ctx);
    void (*delay_us)(void *ctx, uint32_t us);
} i2c_bb_hal_t;

typedef enum {
    I2C_BB_OK = 0,
    I2C_BB_NACK,             /* a byte was not ACKed (device absent or refused)          */
    I2C_BB_BUS_BUSY,         /* SDA or SCL low before START, or SDA low while released  */
    I2C_BB_STRETCH_TIMEOUT,  /* SCL held LOW past the call's I2C_BB_STRETCH_BUDGET_US */
} i2c_bb_status_t;

/* -----------------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------------- */

/**
 * UM10204 §3.1.16 bus clear: release both lines; while SDA is LOW clock SCL up to 9 times; then a
 * STOP. I2C_BB_OK when both lines are high afterwards, I2C_BB_STRETCH_TIMEOUT when SCL never
 * rises, else I2C_BB_BUS_BUSY (SDA still held).
 */
i2c_bb_status_t i2c_bb_bus_clear(const i2c_bb_hal_t *hal);

/** One register write: START, addr7+W, reg, value, STOP. Both lines released on return. */
i2c_bb_status_t i2c_bb_write_reg(const i2c_bb_hal_t *hal, uint8_t addr7, uint8_t reg,
                                 uint8_t value);

/**
 * Run @p p's I2C safe writes: nothing when it has none, else a bus clear and then each write in
 * order. A NACKed write is skipped; a bus fault (busy or stretch timeout) ends the sequence.
 * Returns the number of ACKed writes.
 */
uint8_t i2c_bb_apply_safe_writes(const i2c_bb_hal_t *hal, const board_profile_t *p);

#ifdef __cplusplus
}
#endif

#endif /* I2C_BITBANG_H */
