/* Package v1 wire layout is specified in docs/reference/package-format.md. */
#ifndef UPDATE_PKG_H
#define UPDATE_PKG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UPDATE_PKG_MAGIC "ECBPKG\0\0"
#define UPDATE_PKG_MAGIC_SIZE 8u
#define UPDATE_PKG_VERSION 1u
#define UPDATE_PKG_HEADER_SIZE 32u
#define UPDATE_PKG_ENTRY_SIZE 96u
#define UPDATE_PKG_MAX_NODES 8u
#define UPDATE_PKG_MAX_ENTRIES (UPDATE_PKG_MAX_NODES + 1u)
#define UPDATE_PKG_MANIFEST_MAX (UPDATE_PKG_HEADER_SIZE + UPDATE_PKG_MAX_ENTRIES * UPDATE_PKG_ENTRY_SIZE)
#define UPDATE_PKG_SHA_SIZE 32u
#define UPDATE_PKG_NAME_MAX 32u
#define UPDATE_PKG_ALIGNMENT 4u
#define UPDATE_PKG_CHECK_OFFSET 24u
#define UPDATE_PKG_CAN_ID_MAX 0x7FFu
#define UPDATE_PKG_BRAIN 1u
#define UPDATE_PKG_NODE 2u
/* Current ESP32-S3 deployment limits, checked by the caller, not the parser. */
#define UPDATE_PKG_NODE_SLOT_SIZE 0x1C0000u
#define UPDATE_PKG_BRAIN_SLOT_SIZE 0x300000u
#define UPDATE_PKG_PARTITION_SIZE 0x7E0000u

typedef struct {
    uint8_t type;
    uint8_t name_len;
    uint16_t cmd_id;
    uint16_t reply_id;
    uint32_t offset;
    uint32_t size;
    uint8_t sha256[UPDATE_PKG_SHA_SIZE];
    char name[UPDATE_PKG_NAME_MAX + 1u];
} update_pkg_entry_t;

typedef struct {
    uint32_t total_size;
    uint32_t check;
    uint16_t count;
    update_pkg_entry_t entries[UPDATE_PKG_MAX_ENTRIES];
} update_pkg_t;

typedef enum {
    UPDATE_PKG_OK = 0,
    UPDATE_PKG_TRUNCATED,
    UPDATE_PKG_MAGIC_ERROR,
    UPDATE_PKG_VERSION_ERROR,
    UPDATE_PKG_LAYOUT_ERROR,
    UPDATE_PKG_CHECK_ERROR,
    UPDATE_PKG_ENTRY_ERROR,
    UPDATE_PKG_PAYLOAD_ERROR,
    UPDATE_PKG_NAME_ERROR,
    UPDATE_PKG_CAN_ID_ERROR,
} update_pkg_status_t;

/* buf need contain only header+table; available is its readable length.
 * package_size is the actual stored/uploaded file length, not flash capacity.
 * No payload reads or SHA computation. out is usable ONLY on UPDATE_PKG_OK.
 * Bytewise decoding supports unaligned buffers and all host endiannesses. */
update_pkg_status_t update_pkg_parse(const uint8_t *buf, size_t available,
                                     uint32_t package_size, update_pkg_t *out);
const char *update_pkg_status_text(update_pkg_status_t status);

#ifdef __cplusplus
}
#endif

#endif
