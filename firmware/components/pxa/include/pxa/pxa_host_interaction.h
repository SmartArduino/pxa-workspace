/* SPDX-License-Identifier: MIT */
#ifndef PXA_HOST_INTERACTION_H
#define PXA_HOST_INTERACTION_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define PXA_INTERACTION_MAX_BYTES 8192
#define PXA_INTERACTION_TAP 1u
#define PXA_INTERACTION_PRESS 2u
#define PXA_INTERACTION_RELEASE 4u
#define PXA_INTERACTION_HOLD 8u
#define PXA_INTERACTION_REPEAT 16u
#define PXA_INTERACTION_DOUBLE_TAP 32u
typedef struct { const char *id, *label; } pxa_interaction_control_t;
typedef struct {
    const char *action, *control;
    uint32_t events;
    /* -2 unknown; -1 unbounded; 0 unavailable; positive bounded duration. */
    int32_t hold_limit_ms;
    /* -1 unknown. A press edge is not automatically an atomic tap. */
    int32_t tap_latency_ms;
} pxa_interaction_binding_t;
typedef struct {
    const char *id, *kind;
    const pxa_interaction_binding_t *bindings;
    size_t binding_count;
} pxa_interaction_mode_t;
typedef struct { const char *const *controls; size_t count; } pxa_interaction_exclusive_t;
typedef struct { const char *control, *event, *action; uint32_t threshold_ms; } pxa_interaction_reserved_t;
typedef struct {
    uint32_t struct_size;
    const char *vendor, *model, *hardware_revision, *firmware_id, *firmware_version, *build_id;
    const char *profile_id;
    uint32_t revision;
    uint16_t width, height;
    const char *shape;
    /* -1 unknown, 0 absent/unsupported, 1 present/supported. */
    int8_t touch, simultaneous;
    int8_t max_contacts;
    const pxa_interaction_control_t *controls;
    size_t control_count;
    const pxa_interaction_mode_t *modes;
    size_t mode_count;
    const pxa_interaction_exclusive_t *exclusive;
    size_t exclusive_count;
    const pxa_interaction_reserved_t *reserved;
    size_t reserved_count;
    /* Optional read-only product callback: app/native/overlay/unknown. */
    const char *(*runtime_context)(void *context);
    void *context;
} pxa_host_interaction_t;
/* Register once before starting Host/PXADB tasks. Descriptor and all borrowed
 * strings/arrays remain immutable and alive until reboot. No Guest is started. */
bool pxa_host_interaction_register(const pxa_host_interaction_t *descriptor);
/* Writes one bounded JSON snapshot, excluding device serial/MAC/account data.
 * False means unreported/invalid/insufficient capacity; output_size is zero. */
bool pxa_host_interaction_describe(char *output, size_t capacity, size_t *output_size);
#ifdef __cplusplus
}
#endif
#endif
