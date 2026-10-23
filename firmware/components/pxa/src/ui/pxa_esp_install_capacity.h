#ifndef PXA_ESP_INSTALL_CAPACITY_H
#define PXA_ESP_INSTALL_CAPACITY_H

#include "pxa_esp_ui_shell.h"
#include "pxa/volume.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* Display the same admission budget as the planner, excluding other leases.
 * Unknown space must remain unknown, including read-only reused media. */
static inline void pxa_esp_install_volume_capacity(pxa_esp_ui_install_volume_t *out,
                                                   const pxa_volume_info_t *volume) {
    out->free_known = volume && volume->state == PXA_VOLUME_READY && !volume->ambiguous &&
                      (volume->space_known & PXA_VOLUME_FREE_KNOWN);
    out->free_bytes = !out->free_known || volume->reserved_bytes >= volume->free_bytes ? 0 :
                      volume->free_bytes - volume->reserved_bytes;
}

static inline void pxa_esp_install_size_text(char *out, size_t capacity, uint64_t bytes) {
    static const char *const units[] = {"KiB", "MiB", "GiB", "TiB"};
    if (bytes < 1024) { snprintf(out, capacity, "%llu B", (unsigned long long)bytes); return; }
    uint64_t unit = 1024;
    unsigned index = 0;
    while (index < 3 && bytes / unit >= 1024) { unit *= 1024; ++index; }
    uint64_t whole = bytes / unit, fraction = ((bytes % unit) * 100 + unit / 2) / unit;
    if (fraction == 100) { ++whole; fraction = 0; }
    if (whole == 1024 && fraction == 0 && index < 3) { whole = 1; ++index; }
    snprintf(out, capacity, "%llu.%02u %s", (unsigned long long)whole, (unsigned)fraction, units[index]);
}

static inline void pxa_esp_install_capacity_text(char *out, size_t capacity,
    uint64_t required, uint64_t available, bool known, bool chinese) {
    char need[32], free_space[32];
    pxa_esp_install_size_text(need, sizeof(need), required);
    if (known) {
        pxa_esp_install_size_text(free_space, sizeof(free_space), available);
        /* A rounding tie must not hide a one-byte shortage or imply a fit. */
        if (required != available && !strcmp(need, free_space)) {
            snprintf(need, sizeof(need), "%llu B", (unsigned long long)required);
            snprintf(free_space, sizeof(free_space), "%llu B", (unsigned long long)available);
        }
    } else snprintf(free_space, sizeof(free_space), "%s", chinese ? "未知" : "unknown");
    snprintf(out, capacity, chinese ? "需 %s / 可用 %s" : "Need %s / Free %s", need, free_space);
}

#endif
