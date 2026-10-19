/**
 * @file    slcan_codec.h
 * @brief   Pure-logic SLCAN (Lawicel) line parser and encoder.
 *
 * Exactly the subset python-can's `slcan` backend (can/interfaces/slcan.py, python-can 4.x) sends
 * and parses (below), so the SLCAN bridge serves tools/can-flash unchanged.
 *
 * Not here:     The USB and TWAI I/O (examples/slcan_bridge).
 * Invariants:   Lines are terminated by CR, which is not part of the `line` handed to
 *               slcan_parse_line(). Opening with a bitrate, python-can 4.6 sends "C", "S5", ""
 *               (its empty FD data-bitrate code), "O", "O"; shutdown sends "C".
 * Errors:       Calls return slcan_status_t; anything outside the subset is SLCAN_STATUS_ERROR.
 * Depends on:   Pure C over byte buffers: no ESP-IDF includes, host-tested in tests/.
 *
 * @see     slcan_codec.c, test_slcan_codec.c, docs/guides/slcan-bridge.md
 */

/*
 *   host -> adapter            adapter -> host
 *   ""   (bare CR)             SLCAN_ACK; python-can sends one after Sn
 *   C  O  L                    SLCAN_ACK / SLCAN_NAK
 *   S0..S8                     SLCAN_ACK / SLCAN_NAK (bitrate table below)
 *   tIIIL<data> TIIIIIIIIL<data>  SLCAN_TX_ACK_STD / _EXT, or SLCAN_NAK
 *   rIIIL       RIIIIIIIIL     SLCAN_TX_ACK_STD / _EXT, or SLCAN_NAK
 *   V                          Vhhss  (two decimal digits each)
 *   N                          Nxxxx  (four hex digits)
 *   received frame             tIIIL<data> / TIIIIIIIIL<data> / r.. / R..
 *
 * Everything else (sXXYY BTR timing, S9 83.3k, CAN FD d/D/b/B/Y, F, Z, M/m,
 * malformed hex, out-of-range IDs, DLC > 8, trailing bytes) parses as
 * SLCAN_STATUS_ERROR and is answered with SLCAN_NAK. python-can ignores every
 * reply byte, so a NAK only ever matters to a human on a terminal.
 */

#ifndef SLCAN_CODEC_H
#define SLCAN_CODEC_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif


/* -----------------------------------------------------------------------------
 * Wire constants
 * ----------------------------------------------------------------------------- */
#define SLCAN_MAX_DLC            8u
#define SLCAN_STD_ID_MAX         0x7FFu
#define SLCAN_EXT_ID_MAX         0x1FFFFFFFu
#define SLCAN_LINE_MAX           26u   /* longest command, CR excluded: T + 8 + 1 + 16 */
#define SLCAN_FRAME_LINE_MAX     27u   /* longest encoded frame, CR included          */
#define SLCAN_REPLY_LINE_MAX     6u    /* "Vhhss\r" / "Nxxxx\r"                        */

#define SLCAN_ACK                "\r"
#define SLCAN_NAK                "\a"
#define SLCAN_TX_ACK_STD         "z\r"
#define SLCAN_TX_ACK_EXT         "Z\r"


/* -----------------------------------------------------------------------------
 * Status / error codes
 * ----------------------------------------------------------------------------- */
typedef enum {
    SLCAN_STATUS_OK = 0,
    SLCAN_STATUS_ERROR,          /* malformed or unsupported line: answer SLCAN_NAK */
    SLCAN_STATUS_INVALID_ARG,    /* NULL pointer, value out of range, buffer too small */
} slcan_status_t;


/* -----------------------------------------------------------------------------
 * Types
 * ----------------------------------------------------------------------------- */
typedef struct {
    uint32_t id;          /* 11-bit, or 29-bit when extended                    */
    uint8_t  dlc;         /* 0..8 parsed; encode clamps 9..15 to 8 (classic CAN) */
    bool     extended;
    bool     remote;      /* RTR: no data bytes on the wire                     */
    uint8_t  data[SLCAN_MAX_DLC];
} slcan_frame_t;

typedef enum {
    SLCAN_CMD_EMPTY = 0,         /* bare CR                                  */
    SLCAN_CMD_OPEN,              /* O: open the channel, normal mode          */
    SLCAN_CMD_LISTEN,            /* L: open the channel, listen-only          */
    SLCAN_CMD_CLOSE,             /* C                                         */
    SLCAN_CMD_SET_BITRATE,       /* Sn: bitrate_bps                           */
    SLCAN_CMD_TRANSMIT,          /* t / T / r / R: frame                      */
    SLCAN_CMD_VERSION,           /* V                                         */
    SLCAN_CMD_SERIAL,            /* N                                         */
} slcan_cmd_type_t;

typedef struct {
    slcan_cmd_type_t type;
    uint32_t         bitrate_bps;   /* SLCAN_CMD_SET_BITRATE only */
    slcan_frame_t    frame;         /* SLCAN_CMD_TRANSMIT only    */
} slcan_command_t;


/* -----------------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------------- */

/**
 * Parse one line (CR stripped). S0..S8 map to 10k, 20k, 50k, 100k, 125k,
 * 250k, 500k, 750k and 1M bit/s. Hex digits may be upper or lower case.
 *
 * @return SLCAN_STATUS_OK with *out filled, SLCAN_STATUS_ERROR for a line to
 *         NAK, SLCAN_STATUS_INVALID_ARG for NULL arguments.
 */
slcan_status_t slcan_parse_line(const char *line, size_t len, slcan_command_t *out);

/**
 * Encode a frame as the adapter reports a received one, CR included, into
 * out (at least SLCAN_FRAME_LINE_MAX bytes). Not NUL-terminated.
 */
slcan_status_t slcan_encode_frame(const slcan_frame_t *frame, char *out, size_t out_size,
                                  size_t *out_len);

/** Encode the V reply "Vhhss\r" (hw, sw in 0..99), CR included. */
slcan_status_t slcan_encode_version(uint8_t hw_version, uint8_t sw_version, char *out,
                                    size_t out_size, size_t *out_len);

/** Encode the N reply "Nxxxx\r" (serial as four upper-case hex digits), CR included. */
slcan_status_t slcan_encode_serial(uint16_t serial, char *out, size_t out_size,
                                   size_t *out_len);


#ifdef __cplusplus
}
#endif

#endif /* SLCAN_CODEC_H */
