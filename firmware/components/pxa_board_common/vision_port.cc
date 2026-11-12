// SPDX-License-Identifier: Apache-2.0
#include <pxa_board_common/vision_port.h>
#include <pxa_board_common/peripheral_backends.h>
#include <pxa/pxa_esp_resource_memory.h>
#include <pxa/pxa_worker_notifications.h>
#include <pxa/pxa_host.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <esp_timer.h>
#include <esp_log.h>
#include <algorithm>
#include <cstring>

#ifndef PXA_BOARD_VISION_STACK_BYTES
#define PXA_BOARD_VISION_STACK_BYTES 8192
#endif
#ifndef PXA_BOARD_VISION_STACK_EXTERNAL
#define PXA_BOARD_VISION_STACK_EXTERNAL 0
#endif
#if PXA_BOARD_VISION_STACK_EXTERNAL
#include <esp_heap_caps.h>
#if !CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
#error "An external vision stack requires FreeRTOS external-stack support"
#endif
#endif
static_assert(PXA_BOARD_VISION_STACK_BYTES >= 4096);
namespace {
struct State {
    PxaBoardVisionDriver driver{};
    bool live = false, closing = false, prepared = false, allocated = false;
    bool requested = false, preview_requested = false, preview_enabled = false;
    pxa_raster_asset_t* image = nullptr;
    size_t jpeg_size = 0;
    bool reuse_jpeg = false;
    pxa_status_t status = PXA_STATUS_WOULD_BLOCK;
    uint16_t format = 0;
    const pxa_camera_profile_t* profile = nullptr;
    uint32_t sequence = 0;
    uint8_t* bytes = nullptr;
    void* storage = nullptr;
    size_t capacity = 0, used = 0, offset = 0;
    SemaphoreHandle_t mutex = nullptr;
    TaskHandle_t task = nullptr;
    StaticSemaphore_t mutex_storage;
    StaticTask_t task_storage;
#if PXA_BOARD_VISION_STACK_EXTERNAL
    StackType_t* stack = nullptr;
#else
    StackType_t stack[PXA_BOARD_VISION_STACK_BYTES / sizeof(StackType_t)];
#endif
    PxaWorkerNotifications notifications;
} vision;
void Unlock() { xSemaphoreGive(vision.mutex); }
void Notify() { (void)vision.notifications.Notify(vision.task); }
pxa_status_t List(void*, pxa_peripheral_port_t* ports, size_t capacity, size_t* count) {
    *count = unsigned(vision.driver.jpeg) + unsigned(vision.driver.inference);
    if (capacity < *count) return PXA_STATUS_LIMIT_EXCEEDED;
    size_t i = 0;
    for (const unsigned format : {PXA_VISION_JPEG, PXA_VISION_JSON}) {
        if (format == PXA_VISION_JPEG ? !vision.driver.jpeg : !vision.driver.inference) continue;
        ports[i] = {};
        ports[i].id = format == PXA_VISION_JPEG ? 3 : 4;
        ports[i].kind = PXA_PERIPHERALS_VISION;
        ports[i].flags = PXA_PERIPHERALS_READ;
        std::strcpy(ports[i++].name, format == PXA_VISION_JPEG ? "camera.jpeg" : "vision.inference");
    }
    return PXA_STATUS_OK;
}
pxa_status_t Module(void*, uint16_t, pxa_peripheral_module_t*) { return PXA_STATUS_NOT_FOUND; }
pxa_status_t Profiles(void*, uint16_t port, pxa_camera_profile_t* output, size_t capacity, size_t* count) {
    *count = 0;
    if (port != 3 || !vision.driver.jpeg || !vision.driver.profiles) return PXA_STATUS_UNSUPPORTED;
    if (capacity < vision.driver.profile_count) return PXA_STATUS_LIMIT_EXCEEDED;
    *count = vision.driver.profile_count;
    std::copy_n(vision.driver.profiles, *count, output);
    return PXA_STATUS_OK;
}
pxa_status_t Open(void*, uint64_t owner, uint16_t port, uint16_t mode,
    uint32_t parameter, uint16_t address, void** output) {
    *output = nullptr;
    if (!owner || mode != PXA_PERIPHERALS_MODE_STREAM || address ||
        (port != 3 && port != 4)) return PXA_STATUS_INVALID_ARGUMENT;
    if (port == 3 ? !vision.driver.jpeg : !vision.driver.inference) return PXA_STATUS_UNSUPPORTED;
    const pxa_camera_profile_t* profile = nullptr;
    if (port == 3) {
        for (size_t i = 0; i < vision.driver.profile_count; ++i) {
            const auto* candidate = vision.driver.profiles + i;
            if (parameter ? candidate->id == parameter : (candidate->flags & PXA_CAMERA_PROFILE_DEFAULT)) profile = candidate;
        }
    }
    if (parameter && !profile) return PXA_STATUS_UNSUPPORTED;
    if (!xSemaphoreTake(vision.mutex, 0)) return PXA_STATUS_WOULD_BLOCK;
    if (vision.live || vision.closing) { Unlock(); return PXA_STATUS_BUSY; }
    vision.live = true; vision.status = PXA_STATUS_WOULD_BLOCK;
    vision.format = port == 3 ? PXA_VISION_JPEG : PXA_VISION_JSON;
    vision.profile = profile;
    vision.requested = !vision.driver.preview && !vision.driver.preview_jpeg;
    *output = &vision;
    Unlock(); Notify(); return PXA_STATUS_OK;
}
pxa_status_t Ready(void*, void* device) {
    if (device != &vision) return PXA_STATUS_INVALID_ARGUMENT;
    if (!xSemaphoreTake(vision.mutex, 0)) return PXA_STATUS_WOULD_BLOCK;
    const auto result = vision.live ? vision.status : PXA_STATUS_UNAVAILABLE;
    Unlock(); return result;
}
int32_t Io(void*, void* device, uint32_t operation, uint8_t* bytes, size_t size) {
    if (device != &vision || !bytes || !size || size > PXA_PERIPHERALS_MAX_TRANSFER)
        return PXA_STATUS_INVALID_ARGUMENT;
    if (operation != PXA_IO_READ) return PXA_STATUS_UNSUPPORTED;
    if (!xSemaphoreTake(vision.mutex, 0)) return PXA_STATUS_WOULD_BLOCK;
    if (!vision.live) { Unlock(); return PXA_STATUS_UNAVAILABLE; }
    if (vision.status != PXA_STATUS_OK) { const auto error = vision.status; Unlock(); return error; }
    if (!vision.used) {
        // A caller may record several JPEGs without consuming every preview.
        auto* stale = vision.image;
        vision.image = nullptr;
        vision.requested = true;
        Unlock(); pxa_raster_asset_release(stale); Notify(); return PXA_STATUS_WOULD_BLOCK;
    }
    const auto copied = std::min(size, vision.used - vision.offset);
    std::memcpy(bytes, vision.bytes + vision.offset, copied);
    vision.offset += copied;
    const bool drained = vision.offset == vision.used;
    if (drained) vision.offset = vision.used = 0;
    Unlock();
    if (drained && !vision.driver.preview && !vision.driver.preview_jpeg) Notify();
    return static_cast<int32_t>(copied);
}
pxa_status_t Start(void*, void*, uint64_t, const uint8_t*, uint16_t, uint16_t) { return PXA_STATUS_UNSUPPORTED; }
pxa_status_t Poll(void*, void*, uint64_t, uint8_t*, size_t*) { return PXA_STATUS_UNSUPPORTED; }
void Cancel(void*, void*, uint64_t) {}
void Close(void*, void* device) {
    if (device != &vision) return;
    xSemaphoreTake(vision.mutex, portMAX_DELAY);
    vision.live = false; vision.closing = true;
    Unlock(); Notify();
}
pxa_status_t Preview(void*, void* device, pxa_raster_asset_t** output) {
    *output = nullptr;
    if (device != &vision || (!vision.driver.preview && !vision.driver.preview_jpeg) ||
        vision.format != PXA_VISION_JPEG) return PXA_STATUS_UNSUPPORTED;
    if (!xSemaphoreTake(vision.mutex, 0)) return PXA_STATUS_WOULD_BLOCK;
    if (!vision.live) { Unlock(); return PXA_STATUS_UNAVAILABLE; }
    if (vision.status != PXA_STATUS_OK) { const auto status=vision.status; Unlock(); return status; }
    if (vision.used || vision.requested) { Unlock(); return PXA_STATUS_BUSY; }
    vision.preview_enabled = true;
    if (vision.image) {
        *output=vision.image; vision.image=nullptr;
        Unlock(); return PXA_STATUS_OK;
    }
    vision.preview_requested=true;
    Unlock(); Notify(); return PXA_STATUS_WOULD_BLOCK;
}
pxa_status_t MakePreview(pxa_raster_asset_t** image) {
    if (vision.driver.preview) return vision.driver.preview(vision.driver.context,image);
    auto status = PXA_STATUS_OK;
    if (!vision.reuse_jpeg) {
        status=vision.driver.read(vision.driver.context,vision.bytes+PXA_VISION_HEADER_BYTES,
            vision.capacity-PXA_VISION_HEADER_BYTES,&vision.jpeg_size);
    }
    if (status==PXA_STATUS_OK && (!vision.jpeg_size || vision.jpeg_size>vision.capacity-PXA_VISION_HEADER_BYTES))
        status=PXA_STATUS_PROTOCOL_ERROR;
    if (status==PXA_STATUS_OK) status=vision.driver.preview_jpeg(vision.driver.context,
        vision.bytes+PXA_VISION_HEADER_BYTES,vision.jpeg_size,image);
    if (status==PXA_STATUS_OK) vision.reuse_jpeg=false;
    return status;
}
// Exactly one worker calls Pump. Publication and at-most-256-byte reads use
// the short mutex; capture, allocation and release happen outside it. A close
// reserves this physical session until the old driver and memory are gone.
bool Pump() {
    xSemaphoreTake(vision.mutex, portMAX_DELAY);
    if (vision.closing) {
        Unlock();
        if (vision.allocated) vision.driver.finish(vision.driver.context);
        pxa_raster_asset_release(vision.image);
        pxa_memory_release(vision.storage);
        xSemaphoreTake(vision.mutex, portMAX_DELAY);
        vision.bytes = nullptr; vision.storage = nullptr; vision.used = vision.offset = vision.capacity = 0;
        vision.image = nullptr; vision.requested = vision.preview_requested = false;
        vision.preview_enabled = false;
        vision.jpeg_size=0; vision.reuse_jpeg=false;
        vision.prepared = vision.allocated = vision.closing = false;
        Unlock(); pxa_host_io_notify(); return false;
    }
    if (!vision.live || (vision.status != PXA_STATUS_OK && vision.status != PXA_STATUS_WOULD_BLOCK) || vision.used || vision.image) {
        Unlock(); return false;
    }
    const bool prepare = !vision.prepared;
    const bool preview = vision.preview_requested;
    if (!prepare && (vision.driver.preview || vision.driver.preview_jpeg) && !preview && !vision.requested) { Unlock(); return false; }
    vision.requested = false;
    const auto format = vision.format;
    Unlock();
    if (prepare) {
        vision.capacity = (format == PXA_VISION_JPEG ?
            (vision.profile ? vision.profile->max_frame_bytes : PXA_VISION_MAX_JPEG) :
            PXA_VISION_MAX_JSON) + PXA_VISION_HEADER_BYTES;
        const auto* allocator = pxa_esp_device_resource_allocator(PXA_MEMORY_EXTERNAL, PXA_MEMORY_FRAME);
        // Align the payload, not its wire header, for direct hardware JPEG DMA.
        vision.storage = pxa_memory_allocate(allocator, vision.capacity + 127u);
        vision.bytes = vision.storage ? reinterpret_cast<uint8_t*>(
            (reinterpret_cast<uintptr_t>(vision.storage) + PXA_VISION_HEADER_BYTES + 127u) & ~uintptr_t(127u))
            - PXA_VISION_HEADER_BYTES : nullptr;
        vision.allocated = true; // finish also rolls back partial preparation.
        const auto status = !vision.bytes ? PXA_STATUS_RESOURCE_LIMIT :
            (vision.profile && vision.driver.prepare_profile ?
                vision.driver.prepare_profile(vision.driver.context, *vision.profile) :
                vision.driver.prepare(vision.driver.context, format));
        xSemaphoreTake(vision.mutex, portMAX_DELAY);
        vision.prepared = status == PXA_STATUS_OK;
        vision.status = status;
        const bool again = vision.closing || vision.prepared;
        Unlock(); pxa_host_io_notify(); return again;
    }
    if (preview) {
        pxa_raster_asset_t* image = nullptr;
        const auto status = MakePreview(&image);
        xSemaphoreTake(vision.mutex, portMAX_DELAY);
        vision.preview_requested = false;
        const bool closing = vision.closing;
        vision.status = status;
        if (!closing && status == PXA_STATUS_OK) { vision.image=image; image=nullptr; }
        Unlock(); pxa_raster_asset_release(image); pxa_host_io_notify(); return closing;
    }
    size_t size = 0;
    auto status = vision.driver.read(vision.driver.context, vision.bytes + PXA_VISION_HEADER_BYTES,
                                    vision.capacity - PXA_VISION_HEADER_BYTES, &size);
    const pxa_vision_frame_info_t info{format, static_cast<uint32_t>(size), ++vision.sequence,
                                     static_cast<uint64_t>(esp_timer_get_time())};
    if (status == PXA_STATUS_OK && (size > vision.capacity - PXA_VISION_HEADER_BYTES ||
        size > UINT32_MAX || !pxa_vision_encode(vision.bytes, &info)))
        status = PXA_STATUS_PROTOCOL_ERROR;
    // Drivers must return a frame or a terminal error after their deadline;
    // a WOULD_BLOCK result here is not a busy-loop retry contract.
    if (status == PXA_STATUS_WOULD_BLOCK) status = PXA_STATUS_UNAVAILABLE;
    if (status != PXA_STATUS_OK) ESP_LOGW("PxaVision", "Capture failed: %ld", long(status));
    xSemaphoreTake(vision.mutex, portMAX_DELAY);
    bool closing = vision.closing;
    if (!closing) {
        // Read polling while capture ran requested this same frame.
        vision.requested = false;
        vision.status = status;
        vision.jpeg_size=size; vision.reuse_jpeg=status==PXA_STATUS_OK;
        if (status == PXA_STATUS_OK) { vision.used = PXA_VISION_HEADER_BYTES + size; vision.offset = 0; }
    }
    const bool prefetch = !closing && status==PXA_STATUS_OK && vision.preview_enabled && format==PXA_VISION_JPEG;
    Unlock();
    pxa_host_io_notify();
    if (!prefetch) return closing;
    // JPEG bytes are immutable until the worker captures again. Publish them
    // before preparing the image so Guest reads can overlap conversion/decode.
    pxa_raster_asset_t* image = nullptr;
    const auto preview_status = MakePreview(&image);
    xSemaphoreTake(vision.mutex, portMAX_DELAY);
    closing = vision.closing;
    if (!closing && !vision.requested && preview_status==PXA_STATUS_OK) {
        vision.image=image; image=nullptr;
        vision.preview_requested=false;
    }
    // An optional prefetch failure must not invalidate a successful JPEG.
    // A pending explicit preview retries through the ordinary error path.
    const bool again = closing || vision.requested || vision.preview_requested;
    Unlock(); pxa_raster_asset_release(image); pxa_host_io_notify(); return again;
}
void Worker(void*) {
    for (;;) {
        if (Pump()) continue;
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
}
}
bool pxa_board_vision_port_initialize(const PxaBoardVisionDriver* driver) {
    if (!driver || (!driver->jpeg && !driver->inference) || !driver->prepare || !driver->read || !driver->finish || vision.task ||
        driver->profile_count > PXA_CAMERA_MAX_PROFILES || (driver->profile_count && !driver->profiles))
        return false;
    vision.driver = *driver;
    vision.mutex = xSemaphoreCreateMutexStatic(&vision.mutex_storage);
    if (!vision.mutex) return false;
#if PXA_BOARD_VISION_STACK_EXTERNAL
    // Only opt in for drivers whose worker callbacks never write Flash/NVS.
    // Runtime/Flash task stacks and the task control block remain internal.
    vision.stack = static_cast<StackType_t*>(heap_caps_malloc(
        PXA_BOARD_VISION_STACK_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!vision.stack) return false;
#endif
    vision.task = xTaskCreateStatic(Worker, "pxa_vision", PXA_BOARD_VISION_STACK_BYTES,
        nullptr, 2, vision.stack, &vision.task_storage);
    if (!vision.task) {
#if PXA_BOARD_VISION_STACK_EXTERNAL
        heap_caps_free(vision.stack); vision.stack = nullptr;
#endif
        return false;
    }
    const pxa_peripherals_backend_t backend = {sizeof(pxa_peripherals_backend_t), nullptr,
        List, Module, Open, Io, Start, Poll, Cancel, Close, Ready, Profiles, Preview};
    if (pxa_board_add_peripheral_backend(&backend)) return true;
    vTaskDelete(vision.task); vision.task = nullptr;
#if PXA_BOARD_VISION_STACK_EXTERNAL
    heap_caps_free(vision.stack); vision.stack = nullptr;
#endif
    return false;
}
