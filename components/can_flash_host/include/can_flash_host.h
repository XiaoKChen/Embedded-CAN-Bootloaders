/* =============================================================================
 * can_flash_host.h — Allocation-free CAN Update Protocol v1 host
 * =============================================================================
 * Blocking, single-owner API. Use one context per bus and serialize operations.
 * The transport must deliver standard frames from ANY reply ID for discovery.
 * No ESP-IDF types; constants/CRC are shared with the real node engine.
 * ============================================================================= */
#ifndef CAN_FLASH_HOST_H
#define CAN_FLASH_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "can_update.h"
#include "dfu_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CAN_FLASH_HOST_SCAN_FROM 0x700u
#define CAN_FLASH_HOST_SCAN_TO 0x71Fu
#define CAN_FLASH_HOST_ATTEMPTS 4u
#define CAN_FLASH_HOST_ACK_MS 2000u
#define CAN_FLASH_HOST_ERASE_MS 15000u
#define CAN_FLASH_HOST_SCAN_MS 300u
#define CAN_FLASH_HOST_SEND_MS 2000u
#define CAN_FLASH_HOST_QUIET_MS 150u
#define CAN_FLASH_HOST_EARLY_FRAMES 32u
#define CAN_FLASH_HOST_EARLY_MS 1u
#define CAN_FLASH_HOST_DLC 8u
#define CAN_FLASH_HOST_FRAMES_PER_WINDOW \
    ((CAN_UPDATE_WINDOW_SIZE + CAN_UPDATE_DATA_BYTES_PER_FRAME - 1u) / CAN_UPDATE_DATA_BYTES_PER_FRAME)

typedef enum {
    CAN_FLASH_HOST_STATUS_OK = 0,
    CAN_FLASH_HOST_STATUS_INVALID_ARG,
    CAN_FLASH_HOST_STATUS_TRANSPORT,
    CAN_FLASH_HOST_STATUS_TIMEOUT,
    CAN_FLASH_HOST_STATUS_NAK,
    CAN_FLASH_HOST_STATUS_PROTOCOL,
    CAN_FLASH_HOST_STATUS_READ,
    CAN_FLASH_HOST_STATUS_CAPACITY,
} can_flash_host_status_t;

typedef enum {
    CAN_FLASH_HOST_IO_OK = 0,
    CAN_FLASH_HOST_IO_TIMEOUT,
    CAN_FLASH_HOST_IO_ERROR,
} can_flash_host_io_t;

typedef enum {
    CAN_FLASH_HOST_STEP_NONE = 0,
    CAN_FLASH_HOST_STEP_HELLO = CAN_UPDATE_CMD_HELLO,
    CAN_FLASH_HOST_STEP_ERASE = CAN_UPDATE_CMD_ERASE,
    CAN_FLASH_HOST_STEP_DATA = CAN_UPDATE_CMD_DATA,
    CAN_FLASH_HOST_STEP_SIZE = CAN_UPDATE_CMD_SIZE,
    CAN_FLASH_HOST_STEP_VERIFY = CAN_UPDATE_CMD_VERIFY,
    CAN_FLASH_HOST_STEP_GO = CAN_UPDATE_CMD_GO,
    CAN_FLASH_HOST_STEP_INFO = CAN_UPDATE_CMD_INFO,
} can_flash_host_step_t;

typedef struct {
    uint32_t id;
    uint8_t dlc;
    bool extended;
    bool remote;
    bool error;
    uint8_t data[CAN_FLASH_HOST_DLC];
} can_flash_host_frame_t;

typedef struct {
    can_flash_host_status_t status;
    can_flash_host_step_t step;
    uint8_t nak;
    uint8_t arg;
    uint32_t window;          /* zero-based */
    uint32_t attempts;
    uint32_t bytes;
    uint32_t slot_size;
    uint32_t crc32;
} can_flash_host_result_t;

typedef struct {
    uint16_t cmd_id;
    uint16_t reply_id;
    uint8_t protocol_version;
    uint8_t name_len;
    char name[DFU_CONFIG_STR_MAX + 1u]; /* raw ASCII bytes + NUL; use name_len */
} can_flash_host_info_t;

typedef struct {
    void *user;
    /* send must wait for queue space up to timeout_ms; report permanent errors.
     * recv(0) polls; a timeout advances the clock by the requested interval in
     * simulations. Both callbacks must return within their bounded deadline. */
    can_flash_host_io_t (*send)(void *user, const can_flash_host_frame_t *frame, uint32_t timeout_ms);
    can_flash_host_io_t (*receive)(void *user, can_flash_host_frame_t *frame, uint32_t timeout_ms);
    uint32_t (*clock_ms)(void *user); /* monotonic modulo 2^32 */
    bool (*read)(void *user, uint32_t offset, uint8_t *buf, uint32_t len);
    void (*progress)(void *user, uint32_t accepted_bytes, uint32_t total_bytes);
    can_flash_host_result_t result; /* last operation, including INFO/scan errors */
    uint8_t buffer[CAN_UPDATE_WINDOW_SIZE];
} can_flash_host_t;

/* Zero-initialize context, then assign callbacks. read is required only for
 * flash and must expose immutable bytes throughout the operation (random reads
 * are used for retry and CRC). All output storage is caller-owned. */
can_flash_host_status_t can_flash_host_info(can_flash_host_t *host, uint16_t cmd_id,
                                           uint16_t reply_id, can_flash_host_info_t *info);
/* Probe sorted unique range + legacy 0x79E + known IDs 1/3/5/7/9, just as Python.
 * One 300 ms probe per ID; arbitrary reply pairing. count = stored hits.
 * CAPACITY means more hits exist than max; never writes beyond results[max]. */
can_flash_host_status_t can_flash_host_scan(can_flash_host_t *host, uint16_t from,
                                           uint16_t to, can_flash_host_info_t results[],
                                           uint32_t max, uint32_t *count);
/* HELLO -> SIZE -> ERASE -> DATA -> VERIFY -> GO. Four attempts, 2 s ACK,
 * 15 s ERASE, early NAK poll every frame (1 ms every 32), 150 ms quiet drain.
 * Rewinds resolve only current/next window. Unlike Python, a final DATA timeout
 * probes VERIFY: ACK proves delivery; STATE permits retry; CRC fails closed.
 * Lost SIZE/ERASE/VERIFY ACKs remain fatal on strict-phase nodes. GO timeout is
 * ambiguous (the app may already run); never report success without its ACK.
 * SIZE is sent even when larger than HELLO's slot so the target supplies its
 * authoritative SIZE NAK. No ERASE follows a rejected SIZE.
 * Power loss: host staging is disposable; node boot selection changes only GO.
 */
can_flash_host_status_t can_flash_host_flash(can_flash_host_t *host, uint16_t cmd_id,
                                            uint16_t reply_id, uint32_t image_size,
                                            can_flash_host_result_t *result);
/* Bounded diagnostic, including step, DATA window/attempts and NAK code/name. */
void can_flash_host_result_text(const can_flash_host_result_t *result, char *buf, size_t len);

#ifdef __cplusplus
}
#endif
#endif /* CAN_FLASH_HOST_H */
