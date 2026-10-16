#include "update_pkg.h"

#include <stdbool.h>
#include <string.h>
#include "crc32.h"

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool zero(const uint8_t *p, size_t size)
{
    for (size_t i = 0u; i < size; ++i) {
        if (p[i] != 0u) { return false; }
    }
    return true;
}

update_pkg_status_t update_pkg_parse(const uint8_t *buf, size_t available,
                                     uint32_t package_size, update_pkg_t *out)
{
    if (buf == NULL || out == NULL || available < UPDATE_PKG_HEADER_SIZE) {
        return UPDATE_PKG_TRUNCATED;
    }
    memset(out, 0, sizeof(*out));
    if (memcmp(buf, UPDATE_PKG_MAGIC, UPDATE_PKG_MAGIC_SIZE) != 0) { return UPDATE_PKG_MAGIC_ERROR; }
    if (le16(buf + 8) != UPDATE_PKG_VERSION) { return UPDATE_PKG_VERSION_ERROR; }
    const uint16_t count = le16(buf + 14);
    const uint32_t manifest_size = UPDATE_PKG_HEADER_SIZE + (uint32_t)count * UPDATE_PKG_ENTRY_SIZE;
    if (count == 0u || count > UPDATE_PKG_MAX_ENTRIES ||
        le16(buf + 10) != UPDATE_PKG_HEADER_SIZE || le16(buf + 12) != UPDATE_PKG_ENTRY_SIZE ||
        le32(buf + 16) != manifest_size || le32(buf + 28) != 0u ||
        le32(buf + 20) != package_size || package_size < manifest_size) {
        return UPDATE_PKG_LAYOUT_ERROR;
    }
    if (available < manifest_size) { return UPDATE_PKG_TRUNCATED; }
    static const uint8_t zeros[4] = { 0 };
    uint32_t check = crc32_ieee(buf, UPDATE_PKG_CHECK_OFFSET);
    check = crc32_ieee_update(check, zeros, sizeof(zeros));
    check = crc32_ieee_update(check, buf + UPDATE_PKG_CHECK_OFFSET + sizeof(zeros),
                             manifest_size - UPDATE_PKG_CHECK_OFFSET - (uint32_t)sizeof(zeros));
    if (check != le32(buf + UPDATE_PKG_CHECK_OFFSET)) { return UPDATE_PKG_CHECK_ERROR; }
    uint32_t brains = 0u, nodes = 0u;
    for (uint16_t i = 0u; i < count; ++i) {
        const uint8_t *p = buf + UPDATE_PKG_HEADER_SIZE + (uint32_t)i * UPDATE_PKG_ENTRY_SIZE;
        update_pkg_entry_t *e = &out->entries[i];
        e->type = p[0];
        e->name_len = p[2];
        e->offset = le32(p + 4);
        e->size = le32(p + 8);
        e->cmd_id = le16(p + 12);
        e->reply_id = le16(p + 14);
        if (p[1] != 0u || p[3] != 0u || !zero(p + 80, 16u)) { return UPDATE_PKG_ENTRY_ERROR; }
        if (e->type == UPDATE_PKG_BRAIN) {
            if (++brains > 1u || e->name_len != 0u || e->cmd_id != 0u || e->reply_id != 0u) {
                return UPDATE_PKG_ENTRY_ERROR;
            }
        } else if (e->type == UPDATE_PKG_NODE) {
            if (++nodes > UPDATE_PKG_MAX_NODES) { return UPDATE_PKG_ENTRY_ERROR; }
            if (e->cmd_id > UPDATE_PKG_CAN_ID_MAX || e->reply_id > UPDATE_PKG_CAN_ID_MAX) {
                return UPDATE_PKG_CAN_ID_ERROR;
            }
        } else { return UPDATE_PKG_ENTRY_ERROR; }
        if (e->name_len > UPDATE_PKG_NAME_MAX) { return UPDATE_PKG_NAME_ERROR; }
        for (uint8_t j = 0u; j < e->name_len; ++j) {
            if (p[48u + j] == 0u || p[48u + j] > 0x7Fu) { return UPDATE_PKG_NAME_ERROR; }
        }
        if (!zero(p + 48u + e->name_len, UPDATE_PKG_NAME_MAX - e->name_len)) { return UPDATE_PKG_NAME_ERROR; }
        if (e->offset % UPDATE_PKG_ALIGNMENT != 0u || e->offset < manifest_size ||
            e->size == 0u || e->offset > package_size || e->size > package_size - e->offset) {
            return UPDATE_PKG_PAYLOAD_ERROR;
        }
        for (uint16_t j = 0u; j < i; ++j) {
            const update_pkg_entry_t *prior = &out->entries[j];
            if (e->offset < prior->offset + prior->size && prior->offset < e->offset + e->size) {
                return UPDATE_PKG_PAYLOAD_ERROR;
            }
        }
        memcpy(e->sha256, p + 16, sizeof(e->sha256));
        memcpy(e->name, p + 48, e->name_len);
    }
    out->count = count;
    out->total_size = package_size;
    out->check = check;
    return UPDATE_PKG_OK;
}

const char *update_pkg_status_text(update_pkg_status_t status)
{
    static const char *const texts[] = {
        "ok", "truncated manifest", "bad package magic", "unsupported package version",
        "invalid manifest layout/count/flags/total", "manifest CRC32 mismatch",
        "invalid entry type/count/flags/reserved fields", "invalid payload bounds/alignment/overlap/size",
        "invalid product name", "invalid standard CAN ID",
    };
    return (unsigned)status < sizeof(texts) / sizeof(texts[0]) ? texts[status] : "unknown package error";
}
