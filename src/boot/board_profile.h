/**
 * @file    board_profile.h
 * @brief   Pure-logic parse/validate/encode of the ESP32 board-profile record in cancfg.
 *
 * One bootloader and one CAN updater serve every ESP32-S3 board; what differs per board (TWAI
 * pins, bitrate, GPIOs to hold LOW or HIGH from boot, and a few I2C register writes that force an
 * output chip safe) is data, written by the provisioning tool into the cancfg partition right
 * after the v2 DFU config block. An absent or invalid record => the defaults below, which are
 * the SOLO ODU's TWAI wiring with no holds and no writes, so a board provisioned before this
 * record existed behaves exactly as before.
 *
 * Owns:         The v1 and v2 record layouts (below), their magic, offsets, validity rules and
 *               defaults.
 * Not here:     The v2 DFU config block at cancfg + 0x00 (dfu_config.h); reading flash and
 *               driving pins (node_updater/main/main.c, bootloader_components/main/boot.c,
 *               components/arduino_node_app); the I2C bit-bang that performs the safe writes
 *               (i2c_bitbang.h).
 * Invariants:   SHARED CONTRACT with ESP-PROG-2-PY-CLI (board_profile.py) and the golden vectors
 *               in tests/test_board_profile.c and tests/vectors/board_profile_v2.txt. Change a
 *               value or offset only in lock-step with both. ESP32-only: RA4M1/Embassy readers
 *               stop at byte 0x50 and never see it. A v2 record is refused (version check) by
 *               bootloaders and updaters up to c-cpp-esp32-v1.2.1, which then use the defaults:
 *               provision v2 only together with a bundle that reads it.
 * Errors:       board_profile_parse() returns board_profile_status_t and ALWAYS fills its
 *               out-param (the defaults on failure); the encoders refuse a record that breaks a
 *               rule (or that the v1 layout cannot carry) and leave their buffer untouched.
 * Depends on:   Pure C over a byte buffer (crc32.h only), so it compiles and unit-tests on the
 *               host and in the 2nd-stage bootloader.
 *
 * @see     board_profile.c, test_board_profile.c, docs/reference/board-profile.md
 */

/*
 * Both layouts are little-endian and start at cancfg + BOARD_PROFILE_CANCFG_OFFSET.
 *
 * v1 record (24 bytes):
 *   0x00 u32  magic          = BOARD_PROFILE_MAGIC
 *   0x04 u8   version        = 1
 *   0x05 u8   length         = 24
 *   0x06 u16  reserved       = 0xFFFF (ignored on read)
 *   0x08 u8   twai_tx_gpio   0..48, not refused
 *   0x09 u8   twai_rx_gpio   0..48, not refused, != tx
 *   0x0A u8   bitrate code   board_profile_bitrate_t
 *   0x0B u8   reserved       = 0xFF (ignored on read)
 *   0x0C u64  hold_low_mask  bit n = drive GPIOn LOW as early as possible; no bit above 48,
 *                            no refused pin, no TWAI pin
 *   0x14 u32  crc32          = crc32_ieee over bytes 0x00..0x13
 *
 * v2 record (56 bytes): bytes 0x00..0x13 as v1 except version = 2 and length = 56, then
 *   0x14 u64  hold_high_mask bit n = drive GPIOn HIGH as early as possible; hold_low rules,
 *                            and disjoint from hold_low_mask
 *   0x1C u8   i2c_sda_gpio   0xFF = no I2C safe writes
 *   0x1D u8   i2c_scl_gpio
 *   0x1E u8   i2c_addr7      0x08..0x77
 *   0x1F u8   i2c_write_count 0..8
 *   0x20 8 x {u8 reg, u8 value}  register writes, in order; unused pairs = 0xFF 0xFF
 *                            (ignored on read)
 *   0x30 u32  reserved       = 0xFFFFFFFF (ignored on read)
 *   0x34 u32  crc32          = crc32_ieee over bytes 0x00..0x33
 *
 * v2 I2C rules: sda == 0xFF => scl == 0xFF, addr == 0xFF, count == 0. Otherwise SDA and SCL are
 * 0..48, distinct, not refused, not a TWAI pin and in neither hold mask; addr 0x08..0x77;
 * count 1..8.
 *
 * Refused GPIOs (never TWAI, hold or I2C pins): 0 (BOOT strap), 19/20 (USB D-/D+), 26..32
 * (flash/PSRAM on the WROOM-1U-N16R8 and the S3FH4R2).
 */

#ifndef BOARD_PROFILE_H
#define BOARD_PROFILE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -----------------------------------------------------------------------------
 * Shared contract constants
 * ----------------------------------------------------------------------------- */
#define BOARD_PROFILE_MAGIC              0xDFB0A7D1u
#define BOARD_PROFILE_VERSION_V1         1u
#define BOARD_PROFILE_VERSION_V2         2u
#define BOARD_PROFILE_SIZE_V1            24u
#define BOARD_PROFILE_SIZE_V2            56u
/* The largest record: readers fetch this many bytes and parse() picks the layout. */
#define BOARD_PROFILE_SIZE               BOARD_PROFILE_SIZE_V2
#define BOARD_PROFILE_CRC_COVERAGE_V1    0x14u      /* crc32 over bytes 0x00..0x13       */
#define BOARD_PROFILE_CRC_COVERAGE_V2    0x34u      /* crc32 over bytes 0x00..0x33       */
#define BOARD_PROFILE_CANCFG_OFFSET      0x50u      /* right after the 80-byte v2 block  */
#define BOARD_PROFILE_MAX_GPIO           48u        /* ESP32-S3 GPIO0..GPIO48            */

/* Common to v1 and v2. */
#define BOARD_PROFILE_OFF_MAGIC          0x00u
#define BOARD_PROFILE_OFF_VERSION        0x04u
#define BOARD_PROFILE_OFF_LENGTH         0x05u
#define BOARD_PROFILE_OFF_RESERVED16     0x06u
#define BOARD_PROFILE_OFF_TX             0x08u
#define BOARD_PROFILE_OFF_RX             0x09u
#define BOARD_PROFILE_OFF_BITRATE        0x0Au
#define BOARD_PROFILE_OFF_RESERVED8      0x0Bu
#define BOARD_PROFILE_OFF_HOLD_MASK      0x0Cu      /* hold_low_mask                     */
/* v1 only. */
#define BOARD_PROFILE_OFF_CRC_V1         0x14u
/* v2 only. */
#define BOARD_PROFILE_OFF_HOLD_HIGH_MASK 0x14u
#define BOARD_PROFILE_OFF_I2C_SDA        0x1Cu
#define BOARD_PROFILE_OFF_I2C_SCL        0x1Du
#define BOARD_PROFILE_OFF_I2C_ADDR       0x1Eu
#define BOARD_PROFILE_OFF_I2C_COUNT      0x1Fu
#define BOARD_PROFILE_OFF_I2C_WRITES     0x20u      /* 8 x {reg, value}                  */
#define BOARD_PROFILE_OFF_RESERVED32     0x30u
#define BOARD_PROFILE_OFF_CRC_V2         0x34u

#define BOARD_PROFILE_I2C_NONE           0xFFu      /* i2c_sda/scl/addr: no safe writes  */
#define BOARD_PROFILE_I2C_MAX_WRITES     8u
#define BOARD_PROFILE_I2C_ADDR_MIN       0x08u      /* 7-bit, reserved addresses refused */
#define BOARD_PROFILE_I2C_ADDR_MAX       0x77u

/* GPIO0, 19, 20 and 26..32 as a hold/TWAI/I2C exclusion mask. */
#define BOARD_PROFILE_REFUSED_MASK \
    ((1ull << 0) | (1ull << 19) | (1ull << 20) | (0x7Full << 26))

/* Defaults for an absent or invalid record: the SOLO ODU's TWAI wiring (today's Kconfig values),
 * no holds and no I2C safe writes. */
#define BOARD_PROFILE_DEFAULT_TX_GPIO 4u
#define BOARD_PROFILE_DEFAULT_RX_GPIO 5u
#define BOARD_PROFILE_DEFAULT_BITRATE BOARD_PROFILE_BITRATE_250K
#define BOARD_PROFILE_DEFAULT_HOLD    0ull

typedef enum {
    BOARD_PROFILE_BITRATE_125K = 0,
    BOARD_PROFILE_BITRATE_250K = 1,
    BOARD_PROFILE_BITRATE_500K = 2,
    BOARD_PROFILE_BITRATE_1M   = 3,
} board_profile_bitrate_t;

typedef struct {
    uint8_t reg;
    uint8_t value;
} board_profile_i2c_write_t;

typedef struct {
    uint8_t                   twai_tx_gpio;
    uint8_t                   twai_rx_gpio;
    board_profile_bitrate_t   bitrate;
    uint64_t                  hold_low_mask;
    uint64_t                  hold_high_mask;   /* 0 for a v1 record                   */
    uint8_t                   i2c_sda_gpio;     /* BOARD_PROFILE_I2C_NONE = no writes  */
    uint8_t                   i2c_scl_gpio;
    uint8_t                   i2c_addr7;
    uint8_t                   i2c_write_count;  /* i2c_writes[0..count-1] are used     */
    board_profile_i2c_write_t i2c_writes[BOARD_PROFILE_I2C_MAX_WRITES];
} board_profile_t;

/* Result codes, ordered to match the check order. */
typedef enum {
    BOARD_PROFILE_OK = 0,
    BOARD_PROFILE_ERR_SHORT,    /* buffer shorter than the record its version needs      */
    BOARD_PROFILE_ERR_MAGIC,    /* magic missing (erased flash, v2-block-only provisioning) */
    BOARD_PROFILE_ERR_VERSION,  /* version neither 1 nor 2                               */
    BOARD_PROFILE_ERR_LENGTH,   /* length != 24 (v1) / 56 (v2)                           */
    BOARD_PROFILE_ERR_CRC,      /* crc32 mismatch                                        */
    BOARD_PROFILE_ERR_RULE,     /* a field breaks a rule (pins, bitrate, masks, I2C)     */
} board_profile_status_t;

/* -----------------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------------- */

/** Fill @p out with the defaults (TX 4, RX 5, 250 kbit/s, no holds, no I2C writes). */
void board_profile_defaults(board_profile_t *out);

/**
 * Parse and validate a v1 or v2 record at @p blob (the caller passes
 * cancfg + BOARD_PROFILE_CANCFG_OFFSET and BOARD_PROFILE_SIZE bytes).
 *
 * ALWAYS writes @p out: the decoded record on BOARD_PROFILE_OK, the defaults otherwise. A v1
 * record decodes with hold_high_mask 0 and no I2C writes. Bytes past the record's length are
 * ignored. Pure: no hardware access.
 */
board_profile_status_t board_profile_parse(const uint8_t *blob, size_t len,
                                           board_profile_t *out);

/** BOARD_PROFILE_OK if @p p obeys every v2 field rule, else BOARD_PROFILE_ERR_RULE. */
board_profile_status_t board_profile_check(const board_profile_t *p);

/**
 * Encode @p p as a v2 record into @p out (BOARD_PROFILE_SIZE_V2 bytes, CRC included). Returns
 * BOARD_PROFILE_ERR_RULE and leaves @p out untouched if @p p breaks a rule.
 */
board_profile_status_t board_profile_encode(const board_profile_t *p,
                                            uint8_t out[BOARD_PROFILE_SIZE_V2]);

/**
 * Encode @p p as a v1 record into @p out (BOARD_PROFILE_SIZE_V1 bytes), for boards whose
 * bootloader predates v2. Returns BOARD_PROFILE_ERR_RULE and leaves @p out untouched if @p p
 * breaks a rule or needs v2 (a hold_high_mask or I2C writes).
 */
board_profile_status_t board_profile_encode_v1(const board_profile_t *p,
                                               uint8_t out[BOARD_PROFILE_SIZE_V1]);

/** The bitrate in bit/s for @p code (250000 for an unknown code). */
uint32_t board_profile_bitrate_bps(board_profile_bitrate_t code);

#ifdef __cplusplus
}
#endif

#endif /* BOARD_PROFILE_H */
