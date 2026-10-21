#ifndef PXA_SYSTEM_CLOCK_H
#define PXA_SYSTEM_CLOCK_H

#include <stdbool.h>
#include <stdint.h>
#include <pxsys/system_status.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Optional retained clock (external RTC, companion MCU, etc.). Values are UTC
 * Unix seconds. Return false for an unset clock, oscillator failure or I/O error.
 * read_utc seeds the system clock at boot; write_utc receives the current system
 * time on save, including any network/manual correction. Callbacks run in task
 * context, before power rails and peripheral buses are disabled. */
typedef struct {
    uint32_t struct_size;
    void* context;
    bool (*read_utc)(void* context, int64_t* seconds);
    bool (*write_utc)(void* context, int64_t seconds);
} pxa_system_clock_source_t;

/* Register during board initialization, before initialize(). The descriptor is
 * copied; context must remain alive. NULL selects the system RTC + NVS default.
 * A source needs read_utc; write_utc is optional for a read-only time source. */
bool pxa_system_clock_set_source(const pxa_system_clock_source_t* source);

/* Call once after NVS, timezone and board/RTC initialization, before the shell.
 * Priority: registered RTC, retained system time, NVS, 2026-01-01 00:00 local.
 * The system clock then ticks normally, with or without network time sync.
 * Repeated calls are harmless and never rewind a running clock. */
bool pxa_system_clock_initialize(void);

/* Update only the time/date fields, preserving the other board status fields.
 * Call from the board's regular status timer as well as for the initial status. */
bool pxa_system_clock_update_status(pxsys_system_status_snapshot_t* status);

/* Save UTC to NVS and, when supplied, the retained clock's write_utc callback.
 * Call before physically cutting power or entering deep sleep. esp_restart()
 * is covered automatically by the registered shutdown handler. No periodic
 * flash writes are performed. Returns false if either required write fails. */
bool pxa_system_clock_save(void);

#ifdef __cplusplus
}
#endif

#endif
