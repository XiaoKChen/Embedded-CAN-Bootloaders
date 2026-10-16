/* boot_counter.h — Portable recovery state; no hardware access. */
#ifndef BOOT_COUNTER_H
#define BOOT_COUNTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BOOT_COUNTER_LIMIT 3u
#define BOOT_COUNTER_STABLE_MS 10000u
#define BOOT_COUNTER_PARTITION_LABEL "bootcnt"
#define BOOT_COUNTER_PARTITION_SUBTYPE 0x42u
#define BOOT_COUNTER_FLASH_BYTES 4096u
#define BOOT_COUNTER_RECORD_BYTES 16u
#define BOOT_COUNTER_NO_SLOT SIZE_MAX

typedef struct {
    uint32_t magic;
    uint32_t count;
    uint32_t clean_restart;
    uint32_t check;
} boot_counter_rtc_t;

typedef enum {
    BOOT_COUNTER_POWER_ON,
    BOOT_COUNTER_SOFTWARE,
    BOOT_COUNTER_CRASH,
    BOOT_COUNTER_SLEEP
} boot_counter_reset_t;

typedef struct {
    uint32_t count;
    size_t next_slot;
    bool dirty;
} boot_counter_log_t;

void boot_counter_rtc_clear(boot_counter_rtc_t *rtc);
bool boot_counter_rtc_valid(const boot_counter_rtc_t *rtc);
void boot_counter_rtc_clean_restart(boot_counter_rtc_t *rtc);
bool boot_counter_rtc_boot(boot_counter_rtc_t *rtc, boot_counter_reset_t reset);
/* Exact 128-bit records count; torn records consume space but do not count.
 * A full untripped log may be erased and restarted; a tripped one never wraps. */
boot_counter_log_t boot_counter_log_scan(const void *data, size_t size);
bool boot_counter_log_wrap(const boot_counter_log_t *log);
extern const uint32_t boot_counter_record[BOOT_COUNTER_RECORD_BYTES / sizeof(uint32_t)];

#endif
