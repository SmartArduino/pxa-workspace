#include "pxa_esp_resource_memory.h"
#include "pxa_esp_assets.h"
#if defined(ESP_PLATFORM)
#include "sdkconfig.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include <string.h>
#ifndef CONFIG_PXA_RESOURCE_INTERNAL_BYTES
#define CONFIG_PXA_RESOURCE_INTERNAL_BYTES (128u * 1024u)
#endif
#ifndef CONFIG_PXA_RESOURCE_EXTERNAL_BYTES
#define CONFIG_PXA_RESOURCE_EXTERNAL_BYTES (2u * 1024u * 1024u)
#endif
#ifndef CONFIG_PXA_RESOURCE_TEMPORARY_INTERNAL_BYTES
#define CONFIG_PXA_RESOURCE_TEMPORARY_INTERNAL_BYTES (16u * 1024u)
#endif
#ifndef CONFIG_PXA_RESOURCE_TEMPORARY_EXTERNAL_BYTES
#define CONFIG_PXA_RESOURCE_TEMPORARY_EXTERNAL_BYTES (512u * 1024u)
#endif
static pxa_memory_budget_t resource_budget;
static pxa_memory_owner_t resource_device_owner;
static pxa_memory_allocator_t resource_device_allocators[PXA_MEMORY_CLASSES][PXA_MEMORY_KINDS];
/* One active package and one retiring display lease. Fixed storage and global
 * quota still bound overlap; a third activation gets RESOURCE_LIMIT until a
 * retiring owner drains. Never wait for an inactive display in stop_active. */
#define RESOURCE_ACTIVATIONS 2u
static struct {
    pxa_memory_owner_t owner;
    pxa_memory_allocator_t allocators[PXA_MEMORY_CLASSES][PXA_MEMORY_KINDS];
} resource_activations[RESOURCE_ACTIVATIONS];
static int resource_active = -1;
static portMUX_TYPE resource_lock = portMUX_INITIALIZER_UNLOCKED;
static int resource_initialized;
static void enter(void *context) { portENTER_CRITICAL((portMUX_TYPE *)context); }
static void leave(void *context) { portEXIT_CRITICAL((portMUX_TYPE *)context); }
static void *raw_allocate(void *context, size_t bytes) {
    unsigned cls = (unsigned)(uintptr_t)context;
    return heap_caps_malloc(bytes, MALLOC_CAP_8BIT |
        (cls == PXA_MEMORY_EXTERNAL ? MALLOC_CAP_SPIRAM : MALLOC_CAP_INTERNAL));
}
static void raw_release(void *context, void *memory) {
    (void)context;
    heap_caps_free(memory);
}
static void request_reclaim(void *context, pxa_memory_owner_t owner, uint8_t cls, size_t needed) {
    (void)context; (void)owner;
    (void)pxa_esp_assets_trim(cls, needed);
}
static pxa_status_t reap_retired(void) {
    pxa_status_t result = PXA_STATUS_OK;
    for (unsigned i = 0; i < RESOURCE_ACTIVATIONS; ++i) {
        if ((int)i == resource_active || !resource_activations[i].owner) continue;
        pxa_status_t status = pxa_memory_owner_close(&resource_budget, resource_activations[i].owner);
        if (status == PXA_STATUS_OK) resource_activations[i].owner = 0;
        else result = status;
    }
    return result;
}
pxa_status_t pxa_esp_resource_memory_initialize(void) {
    const size_t limits[2] = {CONFIG_PXA_RESOURCE_INTERNAL_BYTES, CONFIG_PXA_RESOURCE_EXTERNAL_BYTES};
    if (resource_initialized) return PXA_STATUS_OK;
    const pxa_memory_budget_config_t config = {
        .limit = {limits[0], limits[1]}, .lock_context = &resource_lock,
        .lock = enter, .unlock = leave, .reclaim = request_reclaim,
        .temporary_limit = {CONFIG_PXA_RESOURCE_TEMPORARY_INTERNAL_BYTES,
                            CONFIG_PXA_RESOURCE_TEMPORARY_EXTERNAL_BYTES}
    };
    pxa_status_t status = pxa_memory_budget_init(&resource_budget, &config);
    if (status != PXA_STATUS_OK) return status;
    status = pxa_memory_owner_open(&resource_budget, limits, &resource_device_owner);
    if (status != PXA_STATUS_OK) return status;
    for (unsigned c = 0; c < PXA_MEMORY_CLASSES; ++c)
        for (unsigned k = 0; k < PXA_MEMORY_KINDS; ++k)
            resource_device_allocators[c][k] = (pxa_memory_allocator_t){&resource_budget,
                resource_device_owner, (uint8_t)c, (uint8_t)k, (void *)(uintptr_t)c,
                raw_allocate, raw_release};
    resource_initialized = 1;
    return PXA_STATUS_OK;
}
const pxa_memory_allocator_t *pxa_esp_device_resource_allocator(unsigned cls, unsigned kind) {
    return resource_initialized && cls < PXA_MEMORY_CLASSES && kind < PXA_MEMORY_KINDS
        ? &resource_device_allocators[cls][kind] : NULL;
}
pxa_status_t pxa_esp_resource_memory_begin(void) {
    const size_t limits[2] = {CONFIG_PXA_RESOURCE_INTERNAL_BYTES, CONFIG_PXA_RESOURCE_EXTERNAL_BYTES};
    if (resource_active >= 0) return PXA_STATUS_BUSY;
    pxa_status_t status = pxa_esp_resource_memory_initialize();
    if (status != PXA_STATUS_OK) return status;
    (void)reap_retired();
    unsigned slot;
    for (slot = 0; slot < RESOURCE_ACTIVATIONS && resource_activations[slot].owner; ++slot) {}
    if (slot == RESOURCE_ACTIVATIONS) return PXA_STATUS_RESOURCE_LIMIT;
    status = pxa_memory_owner_open(&resource_budget, limits, &resource_activations[slot].owner);
    if (status != PXA_STATUS_OK) return status;
    for (unsigned c = 0; c < PXA_MEMORY_CLASSES; ++c)
        for (unsigned k = 0; k < PXA_MEMORY_KINDS; ++k)
            resource_activations[slot].allocators[c][k] = (pxa_memory_allocator_t){&resource_budget,
                resource_activations[slot].owner, (uint8_t)c, (uint8_t)k, (void *)(uintptr_t)c,
                raw_allocate, raw_release};
    resource_active = (int)slot;
    return PXA_STATUS_OK;
}
pxa_status_t pxa_esp_resource_memory_end(void) {
    resource_active = -1;
    return reap_retired();
}
const pxa_memory_allocator_t *pxa_esp_resource_allocator(unsigned cls, unsigned kind) {
    return resource_active >= 0 && cls < PXA_MEMORY_CLASSES && kind < PXA_MEMORY_KINDS
        ? &resource_activations[resource_active].allocators[cls][kind] : NULL;
}
void *pxa_esp_resource_allocate(unsigned cls, unsigned kind, size_t bytes) {
    return pxa_memory_allocate(pxa_esp_resource_allocator(cls, kind), bytes);
}
void pxa_esp_resource_memory_stats(pxa_memory_stats_t *stats, size_t *fixed_bytes) {
    if (fixed_bytes) *fixed_bytes = sizeof(resource_budget) + sizeof(resource_activations) + sizeof(resource_active) +
        sizeof(resource_lock) + sizeof(resource_initialized) + sizeof(resource_device_owner) +
        sizeof(resource_device_allocators);
    if (stats) {
        memset(stats, 0, sizeof(*stats));
        if (resource_initialized) (void)pxa_memory_budget_stats(&resource_budget, 0, stats);
    }
}
#endif
