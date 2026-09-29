#ifndef PXA_CODEC_MEMORY_H
#define PXA_CODEC_MEMORY_H
#include "pxa/resource_budget.h"

/* The selected esp_audio_codec objects use these media_lib_sal hooks.
 * ABI: espressif/esp-adf-libs/media_lib_sal/include/media_lib_os.h.
 * Keep a post-link audit: new allocation imports must not bypass this adapter. */
extern "C" {
void *media_lib_module_malloc(const char *module, size_t size);
void *media_lib_module_calloc(const char *module, size_t count, size_t size);
void *media_lib_module_realloc(const char *module, void *memory, size_t size);
void media_lib_free(void *memory);
}

/* Scope is local to the calling task, never a global "current app" lookup.
 * No allocation fallback outside a scope. Allocator descriptors must outlive
 * every returned block. Free uses the block's owner even on another task;
 * realloc with a different scope fails without touching the old block. */
class PxaCodecMemoryScope {
public:
    explicit PxaCodecMemoryScope(const pxa_memory_allocator_t *allocator);
    ~PxaCodecMemoryScope();
    void Set(const pxa_memory_allocator_t *allocator);
    PxaCodecMemoryScope(const PxaCodecMemoryScope&) = delete;
    PxaCodecMemoryScope& operator=(const PxaCodecMemoryScope&) = delete;
private:
    const pxa_memory_allocator_t *previous_;
};
#endif
