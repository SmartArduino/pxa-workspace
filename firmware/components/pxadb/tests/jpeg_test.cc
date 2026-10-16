#include "pxadb_jpeg_encode.h"
#include "pxadb_jpeg_memory.h"
extern "C" void* __wrap_jpeg_calloc_inner(size_t);
extern "C" void* __wrap_jpeg_calloc_align_inner(size_t, int);
#include <esp_heap_caps.h>
#include <esp_jpeg_enc.h>
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {
std::mutex mutex;
std::unordered_map<void*, size_t> allocations;
size_t live = 0, peak = 0;
std::atomic<unsigned> external{0}, internal{0};
bool fail_external = false, fail_open = false;
int opens = 0, closes = 0;
size_t required = 0;
jpeg_error_t failure = JPEG_ERR_NO_MEM;
std::vector<size_t> capacities;

void* allocate(size_t size, size_t alignment, uint32_t caps) {
    if (caps & MALLOC_CAP_SPIRAM) {
        ++external;
        if (fail_external) return nullptr;
    } else ++internal;
    const size_t rounded = (size + alignment - 1) / alignment * alignment;
    void* pointer = std::aligned_alloc(alignment, rounded);
    assert(pointer);
    std::memset(pointer, 0, rounded);
    std::lock_guard lock(mutex);
    allocations[pointer] = rounded;
    live += rounded;
    peak = std::max(peak, live);
    return pointer;
}
void reset() {
    assert(live == 0 && allocations.empty());
    peak = 0; external = internal = 0;
    fail_external = fail_open = false;
    opens = closes = 0; required = 0; capacities.clear();
    failure = JPEG_ERR_NO_MEM;
}
}
extern "C" void* heap_caps_malloc(size_t size, uint32_t caps) {
    return allocate(size, 16, caps);
}
extern "C" void* heap_caps_calloc(size_t n, size_t size, uint32_t caps) {
    return allocate(n * size, 16, caps);
}
extern "C" void* heap_caps_aligned_calloc(size_t alignment, size_t n, size_t size, uint32_t caps) {
    return allocate(n * size, alignment, caps);
}
extern "C" void heap_caps_free(void* pointer) {
    if (!pointer) return;
    std::lock_guard lock(mutex);
    const auto found = allocations.find(pointer);
    assert(found != allocations.end());
    live -= found->second; allocations.erase(found);
    std::free(pointer);
}
extern "C" void* __real_jpeg_calloc_inner(size_t size) {
    return allocate(size, 16, MALLOC_CAP_INTERNAL);
}
extern "C" void* __real_jpeg_calloc_align_inner(size_t size, int alignment) {
    return allocate(size, alignment, MALLOC_CAP_INTERNAL);
}
extern "C" jpeg_error_t jpeg_enc_open(jpeg_enc_config_t* config, jpeg_enc_handle_t* handle) {
    ++opens;
    assert(config->src_type == JPEG_PIXEL_FORMAT_RGB565_LE && config->quality == 70);
    assert(config->subsampling == JPEG_SUBSAMPLE_420 && !config->task_enable);
    if (fail_open) { *handle = nullptr; return JPEG_ERR_NO_MEM; }
    *handle = __wrap_jpeg_calloc_align_inner(8192, 16);
    return *handle ? JPEG_ERR_OK : JPEG_ERR_NO_MEM;
}
extern "C" jpeg_error_t jpeg_enc_process(jpeg_enc_handle_t, const uint8_t*, int,
                                         uint8_t* output, int capacity, int* size) {
    capacities.push_back(capacity);
    if (failure == JPEG_ERR_BAD_DATA || static_cast<size_t>(capacity) < required) return failure;
    std::memset(output, 0x5a, required);
    *size = required;
    return JPEG_ERR_OK;
}
extern "C" jpeg_error_t jpeg_enc_close(jpeg_enc_handle_t handle) {
    ++closes; heap_caps_free(handle); return JPEG_ERR_OK;
}

int main() {
    heap_caps_free(__wrap_jpeg_calloc_inner(8192));
    assert(internal == 1 && external == 0);
    {
        pxadb::JpegMemoryScope outer;
        heap_caps_free(__wrap_jpeg_calloc_inner(4096));
        { pxadb::JpegMemoryScope inner; heap_caps_free(__wrap_jpeg_calloc_align_inner(8192, 16)); }
        heap_caps_free(__wrap_jpeg_calloc_inner(16));
        fail_external = true;
        heap_caps_free(__wrap_jpeg_calloc_inner(8192));
        fail_external = false;
    }
    heap_caps_free(__wrap_jpeg_calloc_inner(8192));
    assert(internal == 4 && external == 3);
    reset();
    {
        pxadb::JpegMemoryScope scope;
        std::thread other([] { heap_caps_free(__wrap_jpeg_calloc_inner(8192)); });
        other.join();
        assert(internal == 1 && external == 0);
    }
    reset();
    pxadb::TestControlCaptureInfo info;
    info.width = info.height = 192; info.stride_bytes = 384;
    std::vector<uint16_t> pixels(info.width * info.height);
    uint8_t* output = nullptr; size_t size = 0;
    required = 100000;
    assert(pxadb::EncodeCaptureJpeg(pixels.data(), info, &output, &size));
    assert(size == required && opens == 1 && closes == 1);
    assert((capacities == std::vector<size_t>{147456}));
    assert(peak == 147456 + 8192);
    for (size_t i = 0; i < size; ++i) assert(output[i] == 0x5a);
    heap_caps_free(output); output = nullptr;
    reset(); required = 200000;
    assert(!pxadb::EncodeCaptureJpeg(pixels.data(), info, &output, &size));
    assert(opens == 1 && closes == 1 && live == 0);
    reset(); failure = JPEG_ERR_FAIL; required = 200000;
    assert(!pxadb::EncodeCaptureJpeg(pixels.data(), info, &output, &size));
    assert(opens == 1 && closes == 1 && live == 0);
    reset(); failure = JPEG_ERR_BAD_DATA;
    assert(!pxadb::EncodeCaptureJpeg(pixels.data(), info, &output, &size));
    assert(opens == 1 && closes == 1 && live == 0);
    reset(); fail_external = true;
    assert(!pxadb::EncodeCaptureJpeg(pixels.data(), info, &output, &size));
    assert(opens == 1 && closes == 1 && live == 0);
    reset(); fail_open = true;
    assert(!pxadb::EncodeCaptureJpeg(pixels.data(), info, &output, &size));
    assert(opens == 1 && closes == 0 && live == 0);
    reset(); info.width = info.height = UINT32_MAX;
    assert(!pxadb::EncodeCaptureJpeg(pixels.data(), info, &output, &size));
    assert(opens == 0);
    info.width = 16384; info.height = 32768; info.stride_bytes = 32768;
    assert(!pxadb::EncodeCaptureJpeg(pixels.data(), info, &output, &size));
    assert(opens == 0); // One pixel above the encoder's signed output-size ceiling.
    std::puts("PXADB JPEG: scoped allocation, conservative capacity, failures and cleanup passed");
}
