// SPDX-License-Identifier: Apache-2.0
#include "../vision_port.cc"
#include <atomic>
#include <cassert>
#include <cstdlib>
#include <thread>
#include <vector>
#include <cstdio>
static pxa_peripherals_backend_t backend;
static unsigned allocations, preparations, captures, finishes;
static unsigned preview_images, previews;
static std::atomic<unsigned> completion_wakes{0};
extern "C" void pxa_host_io_notify(void) { ++completion_wakes; }
static bool deny_memory, oversized;
static std::atomic<bool> block{false}, entered{false}, released{false};
static std::atomic<bool> preview_block{false}, preview_entered{false}, preview_released{false};
static bool preview_error;
static bool backend_registration_failure;
bool pxa_board_add_peripheral_backend(const pxa_peripherals_backend_t* b) {
    if(backend_registration_failure)return false;
    backend = *b; return true;
}
extern "C" const pxa_memory_allocator_t* pxa_esp_device_resource_allocator(unsigned c, unsigned k) {
    assert(c == PXA_MEMORY_EXTERNAL && k == PXA_MEMORY_FRAME); return nullptr;
}
extern "C" void* pxa_memory_allocate(const pxa_memory_allocator_t*, size_t n) {
    if (deny_memory) return nullptr;
    ++allocations; return std::malloc(n);
}
extern "C" void pxa_memory_release(void* p) { if (p) { --allocations; std::free(p); } }
static pxa_status_t Prepare(void*, uint16_t format) { assert(format == PXA_VISION_JPEG); ++preparations; return PXA_STATUS_OK; }
static pxa_status_t Capture(void*, uint8_t* p, size_t cap, size_t* n) {
    ++captures;
    assert(reinterpret_cast<uintptr_t>(p) % 128 == 0); // Payload is DMA-aligned, header is separate.
    if (block) { entered = true; while (!released) std::this_thread::yield(); }
    *n = oversized ? cap + 1 : 600;
    if (!oversized) for (size_t i = 0; i < *n; ++i) p[i] = uint8_t(i);
    return PXA_STATUS_OK;
}
static void Finish(void*) { ++finishes; }
extern "C" void pxa_asset_object_release(pxa_asset_object_t* image) {
    if (image) { assert(preview_images); --preview_images; std::free(image); }
}
static pxa_status_t PreviewImage(void*, pxa_raster_asset_t** output) {
    if (preview_block) { preview_entered=true; while (!preview_released) std::this_thread::yield(); }
    if (preview_error) return PXA_STATUS_RESOURCE_LIMIT;
    ++previews; ++preview_images;
    *output=static_cast<pxa_raster_asset_t*>(std::malloc(16));
    return PXA_STATUS_OK;
}
static pxa_status_t PreviewJpeg(void* context, const uint8_t* bytes, size_t size, pxa_raster_asset_t** output) {
    assert(size==600 && bytes[0]==0 && bytes[599]==uint8_t(599));
    return PreviewImage(context,output);
}
int main() {
    const PxaBoardVisionDriver driver{nullptr, true, false, Prepare, Capture, Finish};
#if PXA_BOARD_VISION_STACK_EXTERNAL
    pxa_test_heap_fail=true;
    assert(!pxa_board_vision_port_initialize(&driver));
    assert(!vision.stack && !vision.task && !pxa_test_heap_live);
    pxa_test_heap_fail=false;
#endif
    pxa_test_task_create_fail=true;
    assert(!pxa_board_vision_port_initialize(&driver));
    assert(!vision.task);
#if PXA_BOARD_VISION_STACK_EXTERNAL
    assert(!vision.stack && !pxa_test_heap_live);
#endif
    pxa_test_task_create_fail=false;
    backend_registration_failure=true;
    assert(!pxa_board_vision_port_initialize(&driver));
    assert(!vision.task && pxa_test_tasks_deleted==1);
#if PXA_BOARD_VISION_STACK_EXTERNAL
    assert(!vision.stack && !pxa_test_heap_live);
#endif
    backend_registration_failure=false;
    assert(pxa_board_vision_port_initialize(&driver));
    assert(pxa_test_stack_bytes==PXA_BOARD_VISION_STACK_BYTES);
#if PXA_BOARD_VISION_STACK_EXTERNAL
    assert(vision.stack && pxa_test_heap_live==1);
#endif
    assert(!pxa_board_vision_port_initialize(&driver));
    size_t count; pxa_peripheral_port_t port{};
    assert(backend.list(nullptr, &port, 1, &count) == PXA_STATUS_OK && count == 1);
    assert(port.kind == PXA_PERIPHERALS_VISION && port.id == 3 && port.flags == PXA_PERIPHERALS_READ && !strcmp(port.name, "camera.jpeg"));
    void* handle = nullptr; void* next = nullptr;
    assert(backend.open(nullptr, 1, 4, 7, 0, 0, &handle) == PXA_STATUS_UNSUPPORTED);
    assert(backend.open(nullptr, 1, 3, 7, 1, 0, &handle) == PXA_STATUS_UNSUPPORTED);
    assert(backend.open(nullptr, 1, 3, 7, 0, 0, &handle) == PXA_STATUS_OK);
    assert(!preparations && !allocations && !captures);
    assert(backend.ready(nullptr, handle) == PXA_STATUS_WOULD_BLOCK);
    assert(Pump() && allocations == 1 && preparations == 1);
    assert(completion_wakes==1);
    assert(backend.ready(nullptr, handle) == PXA_STATUS_OK);
    uint8_t bytes[256];
    assert(backend.io(nullptr, handle, PXA_IO_READ, bytes, 100) == PXA_STATUS_WOULD_BLOCK);
    assert(!Pump() && captures == 1);
    assert(completion_wakes==2);
    assert(!Pump() && captures == 1); // Never overwrite an unconsumed frame.
    assert(completion_wakes==2); // Idle polling never creates another wake.
    assert(backend.io(nullptr, handle, PXA_IO_WRITE, bytes, 1) == PXA_STATUS_UNSUPPORTED);
    std::vector<uint8_t> frame;
    while (frame.size() < 624) {
        const auto n = backend.io(nullptr, handle, PXA_IO_READ, bytes, 17);
        assert(n > 0); frame.insert(frame.end(), bytes, bytes + n);
    }
    pxa_vision_frame_info_t info{}; assert(pxa_vision_decode(frame.data(), &info));
    assert(info.size == 600 && info.sequence == 1 && info.monotonic_us == 123456789);
    for (size_t i = 0; i < 600; ++i) assert(frame[24 + i] == uint8_t(i));
    block = true;
    std::thread worker([] { assert(Pump()); });
    while (!entered) std::this_thread::yield();
    assert(backend.io(nullptr,handle,PXA_IO_READ,bytes,1)==PXA_STATUS_WOULD_BLOCK);
    backend.close(nullptr, handle); // In-flight capture does not block close.
    assert(backend.open(nullptr, 2, 3, 7, 0, 0, &next) == PXA_STATUS_BUSY);
    assert(backend.io(nullptr, handle, PXA_IO_READ, bytes, 1) == PXA_STATUS_UNAVAILABLE);
    released = true; worker.join(); block = false;
    assert(!Pump() && !allocations && finishes == 1);
    assert(backend.open(nullptr, 2, 3, 7, 0, 0, &next) == PXA_STATUS_OK);
    deny_memory = true; assert(!Pump());
    assert(backend.ready(nullptr, next) == PXA_STATUS_RESOURCE_LIMIT && preparations == 1);
    backend.close(nullptr, next); assert(!Pump() && !allocations);
    deny_memory=false; oversized=false; vision.driver.preview=PreviewImage;
    assert(backend.open(nullptr,4,3,7,0,0,&next)==PXA_STATUS_OK);
    assert(Pump()); const auto previous=captures;
    assert(!Pump() && captures==previous); // Preview-capable drivers remain idle until requested.
    pxa_raster_asset_t* image=nullptr;
    assert(backend.camera_preview(nullptr,next,&image)==PXA_STATUS_WOULD_BLOCK && !image);
    assert(!Pump() && previews==1 && preview_images==1);
    assert(!Pump() && previews==1);
    assert(backend.camera_preview(nullptr,next,&image)==PXA_STATUS_OK && image);
    pxa_raster_asset_release(image); assert(!preview_images);
    assert(backend.io(nullptr,next,PXA_IO_READ,bytes,sizeof(bytes))==PXA_STATUS_WOULD_BLOCK);
    assert(!Pump() && captures==previous+1);
    for (size_t offset=0;offset<624;) {
        const auto n=backend.io(nullptr,next,PXA_IO_READ,bytes,sizeof(bytes)); assert(n>0); offset+=size_t(n);
    }
    assert(!Pump() && captures==previous+1); // Draining JPEG never overlaps the following storage write.
    assert(!vision.requested);
    assert(backend.camera_preview(nullptr,next,&image)==PXA_STATUS_OK && preview_images==1);
    pxa_raster_asset_release(image);
    assert(backend.io(nullptr,next,PXA_IO_READ,bytes,sizeof(bytes))==PXA_STATUS_WOULD_BLOCK);
    assert(!Pump() && preview_images==1); // An unused prefetched image is closed with the session.
    backend.close(nullptr,next); assert(!Pump() && !preview_images && !allocations);
    vision.driver.preview=nullptr;
    vision.driver.preview_jpeg=PreviewJpeg;
    assert(backend.open(nullptr,5,3,7,0,0,&next)==PXA_STATUS_OK && Pump());
    const auto jpeg_captures=captures;
    assert(!Pump() && captures==jpeg_captures);
    assert(backend.camera_preview(nullptr,next,&image)==PXA_STATUS_WOULD_BLOCK);
    assert(!Pump() && captures==jpeg_captures+1);
    assert(backend.camera_preview(nullptr,next,&image)==PXA_STATUS_OK);
    pxa_raster_asset_release(image);
    assert(backend.io(nullptr,next,PXA_IO_READ,bytes,sizeof(bytes))==PXA_STATUS_WOULD_BLOCK);
    assert(!Pump() && captures==jpeg_captures+2);
    for (size_t offset=0;offset<624;) {
        const auto n=backend.io(nullptr,next,PXA_IO_READ,bytes,sizeof(bytes)); assert(n>0); offset+=size_t(n);
    }
    assert(backend.camera_preview(nullptr,next,&image)==PXA_STATUS_OK);
    assert(captures==jpeg_captures+2); // Decode exactly the JPEG just saved by the app.
    pxa_raster_asset_release(image);

    // JPEG delivery proceeds while the same frame's preview is still decoding.
    preview_block=true;
    assert(backend.io(nullptr,next,PXA_IO_READ,bytes,sizeof(bytes))==PXA_STATUS_WOULD_BLOCK);
    std::thread requested_decode_worker([] { assert(!Pump()); });
    while (!preview_entered) std::this_thread::yield();
    for (size_t offset=0;offset<624;) {
        const auto n=backend.io(nullptr,next,PXA_IO_READ,bytes,sizeof(bytes)); assert(n>0); offset+=size_t(n);
    }
    assert(backend.camera_preview(nullptr,next,&image)==PXA_STATUS_WOULD_BLOCK);
    preview_released=true; requested_decode_worker.join();
    assert(!vision.preview_requested);
    assert(backend.camera_preview(nullptr,next,&image)==PXA_STATUS_OK);
    pxa_raster_asset_release(image);

    preview_entered=preview_released=false;
    assert(backend.io(nullptr,next,PXA_IO_READ,bytes,sizeof(bytes))==PXA_STATUS_WOULD_BLOCK);
    std::thread decode_worker([] { assert(Pump()); });
    while (!preview_entered) std::this_thread::yield();
    assert(backend.camera_preview(nullptr,next,&image)==PXA_STATUS_BUSY);
    for (size_t offset=0;offset<624;) {
        const auto n=backend.io(nullptr,next,PXA_IO_READ,bytes,sizeof(bytes)); assert(n>0); offset+=size_t(n);
    }
    // Skipping this preview and requesting the next JPEG cannot stall capture.
    assert(backend.io(nullptr,next,PXA_IO_READ,bytes,sizeof(bytes))==PXA_STATUS_WOULD_BLOCK);
    preview_released=true; decode_worker.join(); preview_block=false;
    assert(!preview_images && vision.requested);
    assert(!Pump() && preview_images==1);
    for (size_t offset=0;offset<624;) {
        const auto n=backend.io(nullptr,next,PXA_IO_READ,bytes,sizeof(bytes)); assert(n>0); offset+=size_t(n);
    }
    assert(backend.io(nullptr,next,PXA_IO_READ,bytes,sizeof(bytes))==PXA_STATUS_WOULD_BLOCK);
    assert(!preview_images); // Also discard an already prepared, skipped image.
    preview_error=true;
    assert(!Pump() && backend.ready(nullptr,next)==PXA_STATUS_OK);
    for (size_t offset=0;offset<624;) {
        const auto n=backend.io(nullptr,next,PXA_IO_READ,bytes,sizeof(bytes)); assert(n>0); offset+=size_t(n);
    }
    assert(backend.camera_preview(nullptr,next,&image)==PXA_STATUS_WOULD_BLOCK);
    assert(!Pump() && backend.ready(nullptr,next)==PXA_STATUS_RESOURCE_LIMIT);
    preview_error=false;
    backend.close(nullptr,next); assert(!Pump() && !preview_images && !allocations);

    // Closing during prefetch releases its result and preserves physical ownership.
    assert(backend.open(nullptr,6,3,7,0,0,&next)==PXA_STATUS_OK && Pump());
    assert(!vision.preview_enabled);
    assert(backend.io(nullptr,next,PXA_IO_READ,bytes,sizeof(bytes))==PXA_STATUS_WOULD_BLOCK);
    assert(!Pump() && !preview_images); // JPEG-only sessions do not allocate previews.
    for (size_t offset=0;offset<624;) {
        const auto n=backend.io(nullptr,next,PXA_IO_READ,bytes,sizeof(bytes)); assert(n>0); offset+=size_t(n);
    }
    assert(backend.camera_preview(nullptr,next,&image)==PXA_STATUS_WOULD_BLOCK && !Pump());
    assert(backend.camera_preview(nullptr,next,&image)==PXA_STATUS_OK);
    pxa_raster_asset_release(image);
    preview_block=true; preview_entered=preview_released=false;
    assert(backend.io(nullptr,next,PXA_IO_READ,bytes,sizeof(bytes))==PXA_STATUS_WOULD_BLOCK);
    std::thread close_decode_worker([] { assert(Pump()); });
    while (!preview_entered) std::this_thread::yield();
    backend.close(nullptr,next);
    assert(backend.open(nullptr,7,3,7,0,0,&handle)==PXA_STATUS_BUSY);
    preview_released=true; close_decode_worker.join(); preview_block=false;
    assert(!Pump() && !preview_images && !allocations);
    vision.driver.preview_jpeg=nullptr;
    assert(backend.open(nullptr, 3, 3, 7, 0, 0, &next) == PXA_STATUS_OK);
    assert(Pump()); oversized = true; assert(!Pump());
    assert(backend.io(nullptr, next, PXA_IO_READ, bytes, 1) == PXA_STATUS_PROTOCOL_ERROR);
    backend.close(nullptr, next); assert(!Pump() && !allocations);
    // A selected HD profile owns its larger bounded, DMA-aligned payload.
    const pxa_camera_profile_t hd{10,PXA_VISION_JPEG,1920,1080,82,PXA_VISION_MAX_JPEG_HD,40000,PXA_CAMERA_PROFILE_DEFAULT};
    vision.driver.profiles=&hd; vision.driver.profile_count=1;
    vision.driver.prepare_profile=[](void* ctx,const pxa_camera_profile_t&) { return Prepare(ctx,PXA_VISION_JPEG); };
    oversized=false;
    assert(backend.open(nullptr,9,3,7,10,0,&next)==PXA_STATUS_OK && Pump());
    assert(vision.capacity==PXA_VISION_MAX_JPEG_HD+PXA_VISION_HEADER_BYTES);
    assert(!Pump());
    backend.close(nullptr,next); assert(!Pump() && !allocations);
    puts("Vision native: permission, bounded memory, immutable JPEG delivery during preview, skipped previews, prefetch failure, capture/close race and cleanup passed");
}
