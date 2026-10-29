// SPDX-License-Identifier: Apache-2.0
#ifndef PXA_EXT_VOICE_TRIGGER_H
#define PXA_EXT_VOICE_TRIGGER_H
#include "voice_trigger_protocol.h"
#include <pxa/permission.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t id;
    char label[PXA_VT_MAX_WORD_BYTES + 1];
} pxa_vt_keyword_t;

typedef struct {
    uint32_t flags;
    uint32_t ready;
    uint32_t revision;
    uint32_t update_mode;
    uint16_t keyword_count;
    pxa_vt_keyword_t keywords[PXA_VT_MAX_KEYWORDS];
} pxa_vt_profile_t;

typedef struct {
    uint64_t sequence;
    uint64_t timestamp_us;
    uint32_t revision;
    uint32_t keyword_id;
} pxa_vt_match_t;

/* All callbacks run on the runtime owner thread and must not block. A device
 * worker queues/copies matches; it never calls the runtime from UART/ISR code.
 * profile() returns a coherent snapshot, including while the engine boots.
 * start() flushes older matches and validates the supplied revision.
 * read() consumes one match, WOULD_BLOCK for empty, another error for failure.
 * stop() must be idempotent and flush the queue. No raw microphone is exposed. */
typedef struct {
    uint32_t struct_size;
    void *context;
    pxa_status_t (*profile)(void *, pxa_vt_profile_t *);
    pxa_status_t (*start)(void *, uint32_t revision);
    pxa_status_t (*read)(void *, pxa_vt_match_t *);
    void (*stop)(void *);
} pxa_vt_backend_t;

typedef struct pxa_vt_service pxa_vt_service_t;
size_t pxa_vt_service_workspace_size(void);
pxa_status_t pxa_vt_service_init(void *workspace, size_t size,
    pxa_runtime_t *runtime, pxa_permission_service_t *permissions,
    const pxa_vt_backend_t *backend, pxa_vt_service_t **output);
pxa_status_t pxa_vt_service_register(pxa_vt_service_t *service);
/* Enforced by Host, independent of guest cooperation. INVALID closes active
 * subscriptions. A background component cannot subscribe. */
void pxa_vt_service_set_foreground(pxa_vt_service_t *, pxa_component_t);
int pxa_vt_service_active(const pxa_vt_service_t *);
/* Call at least every 20 ms while active; bounded work, reliable event retry. */
pxa_status_t pxa_vt_service_poll(pxa_vt_service_t *, uint64_t now_us);
/* Before freeing the workspace, while the runtime is still alive. */
void pxa_vt_service_deinit(pxa_vt_service_t *);
#ifdef __cplusplus
}
#endif
#endif
