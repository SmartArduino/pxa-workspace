#include "pxa_codec_memory.h"
#include <cstring>
#include <cstdint>

namespace {
// Constant-initialized TLS needs no destructor or FreeRTOS pointer slot.
thread_local const pxa_memory_allocator_t *codec_allocator = nullptr;
}
PxaCodecMemoryScope::PxaCodecMemoryScope(const pxa_memory_allocator_t *allocator)
    : previous_(codec_allocator) { codec_allocator = allocator; }
PxaCodecMemoryScope::~PxaCodecMemoryScope() { codec_allocator = previous_; }
void PxaCodecMemoryScope::Set(const pxa_memory_allocator_t *allocator) {
    codec_allocator = allocator;
}
extern "C" void *media_lib_module_malloc(const char *, size_t size) {
    return pxa_memory_allocate(codec_allocator, size);
}
extern "C" void *media_lib_module_calloc(const char *, size_t count, size_t size) {
    if (!count || size > SIZE_MAX / count) return nullptr;
    void *memory = pxa_memory_allocate(codec_allocator, count * size);
    if (memory) std::memset(memory, 0, count * size);
    return memory;
}
extern "C" void *media_lib_module_realloc(const char *, void *memory, size_t size) {
    return pxa_memory_resize(codec_allocator, memory, size);
}
extern "C" void media_lib_free(void *memory) { pxa_memory_release(memory); }
