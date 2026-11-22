// SPDX-License-Identifier: Apache-2.0
#ifndef PXA_DEBUG_INSTALL_H
#define PXA_DEBUG_INSTALL_H
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PXA_DEBUG_INSTALL_RECEIVE = 1,
    PXA_DEBUG_INSTALL_VERIFY,
    PXA_DEBUG_INSTALL_WRITE,
} pxa_debug_install_phase_t;

enum { PXA_DEBUG_INSTALL_NAME_CAPACITY = 129, PXA_DEBUG_INSTALL_VERSION_CAPACITY = 65 };

/* Initialize the display-side polling timer during Host startup. */
void pxa_debug_install_bind(void);
/* Trusted device UI for a debug transport's existing install transaction.
 * A zero token means another transaction owns the screen or it is unavailable. */
uint32_t pxa_debug_install_begin(const char *name, const char *version, uint64_t bytes);
void pxa_debug_install_progress(uint32_t token, pxa_debug_install_phase_t phase,
                                uint64_t received, uint64_t total);
void pxa_debug_install_end(uint32_t token, int32_t status);
bool pxa_debug_install_active(void);

#ifdef __cplusplus
}
#endif
#endif
