#include "pxadb_jpeg_memory.h"
#include <cstddef>
#include <esp_heap_caps.h>

namespace {
thread_local bool prefer_external = false;
constexpr size_t kExternalWorkspaceThreshold = 4096;
}

namespace pxadb {
JpegMemoryScope::JpegMemoryScope() : previous_(prefer_external) {
    prefer_external = true;
}
JpegMemoryScope::~JpegMemoryScope() { prefer_external = previous_; }
}

// esp_new_jpeg 1.0.2 has no per-encoder allocator. Keep its two private hooks
// isolated here; symbol changes fail the link and require revalidation. Both
// memory pools are supported by the vendor; freeing remains heap_caps_free.
extern "C" void* __real_jpeg_calloc_inner(size_t size);
extern "C" void* __real_jpeg_calloc_align_inner(size_t size, int alignment);

extern "C" void* __wrap_jpeg_calloc_inner(size_t size) {
    if (prefer_external && size >= kExternalWorkspaceThreshold) {
        if (auto* memory = heap_caps_calloc(1, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT))
            return memory;
    }
    return __real_jpeg_calloc_inner(size);
}

extern "C" void* __wrap_jpeg_calloc_align_inner(size_t size, int alignment) {
    if (prefer_external && size >= kExternalWorkspaceThreshold && alignment > 0) {
        if (auto* memory = heap_caps_aligned_calloc(alignment, 1, size,
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT))
            return memory;
    }
    return __real_jpeg_calloc_align_inner(size, alignment);
}
