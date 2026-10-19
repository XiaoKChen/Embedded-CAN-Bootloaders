/**
 * @file    board_profile.h
 * @brief   Pure-logic parse/validate/encode of the ESP32 board-profile record in cancfg.
 *
 * One bootloader and one CAN updater serve every ESP32-S3 board; what differs per board (TWAI
 * pins, bitrate, GPIOs to hold LOW from boot) is data, written by the provisioning tool into the
 * cancfg partition right after the v2 DFU config block. An absent or invalid record => the
 * defaults below, which are the SOLO ODU's wiring, so a board provisioned before this record
 * existed behaves exactly as before.
 *
 * Owns:         The record layout (below), its magic, offsets, validity rules and defaults.
 * Not here:     The v2 DFU config block at cancfg + 0x00 (dfu_config.h); reading flash and
 *               driving pins (node_updater/main/main.c, bootloader_components/main/boot.c,
 *               components/arduino_node_app).
 * Invariants:   SHARED CONTRACT with ESP-PROG-2-PY-CLI (board_profile.py) and the golden vectors
 *               in tests/vectors/. Change a value or offset only in lock-step with both.
 *               ESP32-only: RA4M1/Embassy readers stop at byte 0x50 and never see it.
 * Errors:       board_profile_parse() returns board_profile_status_t and ALWAYS fills its
 *               out-param (the defaults on failure); board_profile_encode() refuses a record
 *               that breaks a rule and leaves its buffer untouched.
 * Depends on:   Pure C over a byte buffer (crc32.h only), so it compiles and unit-tests on the
 *               host and in the 2nd-stage bootloader.
 *
 * @see     board_profile.c, test_board_profile.c, docs/reference/board-profile.md
 */

/*
 * v1 record (little-endian, 24 bytes) at cancfg + BOARD_PROFILE_CANCFG_OFFSET:
 *   0x00 u32  magic         = BOARD_PROFILE_MAGIC
 *   0x04 u8   version       = 1
 *   0x05 u8   length        = 24
 *   0x06 u16  reserved      = 0xFFFF (ignored on read)
 *   0x08 u8   twai_tx_gpio  0..48, not refused
 *   0x09 u8   twai_rx_gpio  0..48, not refused, != tx
 *   0x0A u8   bitrate code  board_profile_bitrate_t
 *   0x0B u8   reserved      = 0xFF (ignored on read)
 *   0x0C u64  hold_low_mask bit n = drive GPIOn LOW as early as possible; no bit above 48,
 *                           no refused pin, no TWAI pin
 *   0x14 u32  crc32         = crc32_ieee over bytes 0x00..0x13
 *
 * Refused GPIOs (never TWAI or hold pins): 0 (BOOT strap), 19/20 (USB D-/D+), 26..32
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
#define BOARD_PROFILE_MAGIC           0xDFB0A7D1u
#define BOARD_PROFILE_VERSION         1u
#define BOARD_PROFILE_SIZE            24u
#define BOARD_PROFILE_CRC_COVERAGE    20u        /* crc32 over bytes 0x00..0x13       */
#define BOARD_PROFILE_CANCFG_OFFSET   0x50u      /* right after the 80-byte v2 block  */
#define BOARD_PROFILE_MAX_GPIO        48u        /* ESP32-S3 GPIO0..GPIO48            */

#define BOARD_PROFILE_OFF_MAGIC       0x00u
#define BOARD_PROFILE_OFF_VERSION     0x04u
#define BOARD_PROFILE_OFF_LENGTH      0x05u
#define BOARD_PROFILE_OFF_RESERVED16  0x06u
#define BOARD_PROFILE_OFF_TX          0x08u
#define BOARD_PROFILE_OFF_RX          0x09u
#define BOARD_PROFILE_OFF_BITRATE     0x0Au
#define BOARD_PROFILE_OFF_RESERVED8   0x0Bu
#define BOARD_PROFILE_OFF_HOLD_MASK   0x0Cu
#define BOARD_PROFILE_OFF_CRC         0x14u

/* GPIO0, 19, 20 and 26..32 as a hold/TWAI exclusion mask. */
#define BOARD_PROFILE_REFUSED_MASK \
    ((1ull << 0) | (1ull << 19) | (1ull << 20) | (0x7Full << 26))

/* Defaults for an absent or invalid record: the SOLO ODU's wiring (today's Kconfig values). */
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
    uint8_t                 twai_tx_gpio;
    uint8_t                 twai_rx_gpio;
    board_profile_bitrate_t bitrate;
    uint64_t                hold_low_mask;
} board_profile_t;

/* Result codes, ordered to match the check order. */
typedef enum {
    BOARD_PROFILE_OK = 0,
    BOARD_PROFILE_ERR_SHORT,    /* buffer shorter than BOARD_PROFILE_SIZE              */
    BOARD_PROFILE_ERR_MAGIC,    /* magic missing (erased flash, v2-only provisioning)  */
    BOARD_PROFILE_ERR_VERSION,  /* version != 1                                         */
    BOARD_PROFILE_ERR_LENGTH,   /* length != 24                                         */
    BOARD_PROFILE_ERR_CRC,      /* crc32 mismatch                                       */
    BOARD_PROFILE_ERR_RULE,     /* a field breaks a rule (pins, bitrate, hold mask)     */
} board_profile_status_t;

/* -----------------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------------- */

/** Fill @p out with the defaults (TX 4, RX 5, 250 kbit/s, no holds). */
void board_profile_defaults(board_profile_t *out);

/**
 * Parse and validate a record at @p blob (the caller passes cancfg + BOARD_PROFILE_CANCFG_OFFSET).
 *
 * ALWAYS writes @p out: the decoded record on BOARD_PROFILE_OK, the defaults otherwise.
 * Bytes past BOARD_PROFILE_SIZE are ignored. Pure: no hardware access.
 */
board_profile_status_t board_profile_parse(const uint8_t *blob, size_t len,
                                           board_profile_t *out);

/** BOARD_PROFILE_OK if @p p obeys every field rule, else BOARD_PROFILE_ERR_RULE. */
board_profile_status_t board_profile_check(const board_profile_t *p);

/**
 * Encode @p p into @p out (BOARD_PROFILE_SIZE bytes, CRC included). Returns
 * BOARD_PROFILE_ERR_RULE and leaves @p out untouched if @p p breaks a rule.
 */
board_profile_status_t board_profile_encode(const board_profile_t *p,
                                            uint8_t out[BOARD_PROFILE_SIZE]);

/** The bitrate in bit/s for @p code (250000 for an unknown code). */
uint32_t board_profile_bitrate_bps(board_profile_bitrate_t code);

#ifdef __cplusplus
}
#endif

#endif /* BOARD_PROFILE_H */
