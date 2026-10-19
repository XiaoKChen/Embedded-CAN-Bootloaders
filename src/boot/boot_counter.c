/**
 * @file    boot_counter.c
 * @brief   Validated RTC counter and append-only flash bit log (see boot_counter.h for the
 *          contract).
 *
 * Implementation notes: the RTC record is sealed with a magic and an XOR check over its fields,
 * so garbage retained memory fails boot_counter_rtc_valid(). A log slot is free only when all 16
 * bytes read erased (0xFF); only exact copies of boot_counter_record count.
 *
 * @see     boot_counter.h, test_boot_counter.c
 */
#include "boot_counter.h"

#include <string.h>

#define RTC_MAGIC UINT32_C(0x39435442)
#define RTC_CHECK UINT32_C(0xC38F25A6)

const uint32_t boot_counter_record[] = {
    UINT32_C(0x4E434239), UINT32_C(0xB1BCBDC6),
    UINT32_C(0x65A93C87), UINT32_C(0x9A56C378)
};

static void rtc_seal(boot_counter_rtc_t *rtc)
{
    rtc->magic = RTC_MAGIC;
    rtc->check = RTC_CHECK ^ rtc->magic ^ rtc->count ^ (rtc->clean_restart << 16);
}

bool boot_counter_rtc_valid(const boot_counter_rtc_t *rtc)
{
    return rtc->magic == RTC_MAGIC && rtc->count <= BOOT_COUNTER_LIMIT &&
           rtc->clean_restart <= 1u &&
           rtc->check == (RTC_CHECK ^ rtc->magic ^ rtc->count ^ (rtc->clean_restart << 16));
}

void boot_counter_rtc_clear(boot_counter_rtc_t *rtc)
{
    rtc->count = 0u;
    rtc->clean_restart = 0u;
    rtc_seal(rtc);
}

void boot_counter_rtc_clean_restart(boot_counter_rtc_t *rtc)
{
    if (!boot_counter_rtc_valid(rtc)) {
        boot_counter_rtc_clear(rtc);
    }
    rtc->clean_restart = 1u;
    rtc_seal(rtc);
}

bool boot_counter_rtc_boot(boot_counter_rtc_t *rtc, boot_counter_reset_t reset)
{
    if (reset == BOOT_COUNTER_POWER_ON || !boot_counter_rtc_valid(rtc)) {
        boot_counter_rtc_clear(rtc);
    }
    const bool clean = reset == BOOT_COUNTER_SOFTWARE && rtc->clean_restart != 0u;
    if ((reset == BOOT_COUNTER_CRASH || reset == BOOT_COUNTER_SOFTWARE) && !clean &&
        rtc->count < BOOT_COUNTER_LIMIT) {
        ++rtc->count;
    }
    rtc->clean_restart = 0u; /* One-use mark, not immunity to a later panic. */
    rtc_seal(rtc);
    return rtc->count >= BOOT_COUNTER_LIMIT;
}

boot_counter_log_t boot_counter_log_scan(const void *data, size_t size)
{
    boot_counter_log_t log = {.next_slot = BOOT_COUNTER_NO_SLOT};
    const uint8_t *bytes = data;
    if (bytes == NULL) {
        return log;
    }
    for (size_t offset = 0; offset + BOOT_COUNTER_RECORD_BYTES <= size;
         offset += BOOT_COUNTER_RECORD_BYTES) {
        bool erased = true;
        for (size_t i = 0; i < BOOT_COUNTER_RECORD_BYTES; ++i) {
            if (bytes[offset + i] != UINT8_MAX) {
                erased = false;
                break;
            }
        }
        log.dirty |= !erased;
        if (erased && log.next_slot == BOOT_COUNTER_NO_SLOT) {
            log.next_slot = offset;
        }
        if (memcmp(bytes + offset, boot_counter_record, BOOT_COUNTER_RECORD_BYTES) == 0 &&
            log.count < BOOT_COUNTER_LIMIT) {
            ++log.count;
        }
    }
    return log;
}

bool boot_counter_log_wrap(const boot_counter_log_t *log)
{
    return log->dirty && log->count < BOOT_COUNTER_LIMIT &&
           log->next_slot == BOOT_COUNTER_NO_SLOT;
}
