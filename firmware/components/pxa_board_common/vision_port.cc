// SPDX-License-Identifier: Apache-2.0
#include <pxa_board_common/vision_port.h>
#include <pxa_board_common/peripheral_backends.h>
#include <pxa/pxa_esp_resource_memory.h>
#include <pxa/pxa_worker_notifications.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <esp_timer.h>
#include <esp_log.h>
#include <algorithm>
#include <cstring>

namespace {
struct State {
    PxaBoardVisionDriver driver{};
    bool live = false, closing = false, prepared = false, allocated = false;
    pxa_status_t status = PXA_STATUS_WOULD_BLOCK;
    uint16_t format = 0;
    uint32_t sequence = 0;
    uint8_t* bytes = nullptr;
    size_t capacity = 0, used = 0, offset = 0;
    SemaphoreHandle_t mutex = nullptr;
    TaskHandle_t task = nullptr;
    StaticSemaphore_t mutex_storage;
    StaticTask_t task_storage;
    StackType_t stack[8192 / sizeof(StackType_t)];
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
pxa_status_t Open(void*, uint64_t owner, uint16_t port, uint16_t mode,
    uint32_t parameter, uint16_t address, void** output) {
    *output = nullptr;
    if (!owner || mode != PXA_PERIPHERALS_MODE_STREAM || parameter || address ||
        (port != 3 && port != 4)) return PXA_STATUS_INVALID_ARGUMENT;
    if (port == 3 ? !vision.driver.jpeg : !vision.driver.inference) return PXA_STATUS_UNSUPPORTED;
    if (!xSemaphoreTake(vision.mutex, 0)) return PXA_STATUS_WOULD_BLOCK;
    if (vision.live || vision.closing) { Unlock(); return PXA_STATUS_BUSY; }
    vision.live = true; vision.status = PXA_STATUS_WOULD_BLOCK;
    vision.format = port == 3 ? PXA_VISION_JPEG : PXA_VISION_JSON;
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
    if (!vision.used) { Unlock(); return PXA_STATUS_WOULD_BLOCK; }
    const auto copied = std::min(size, vision.used - vision.offset);
    std::memcpy(bytes, vision.bytes + vision.offset, copied);
    vision.offset += copied;
    const bool drained = vision.offset == vision.used;
    if (drained) vision.offset = vision.used = 0;
    Unlock();
    if (drained) Notify();
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
// Exactly one worker calls Pump. Publication and at-most-256-byte reads use
// the short mutex; capture, allocation and release happen outside it. A close
// reserves this physical session until the old driver and memory are gone.
bool Pump() {
    xSemaphoreTake(vision.mutex, portMAX_DELAY);
    if (vision.closing) {
        Unlock();
        if (vision.allocated) vision.driver.finish(vision.driver.context);
        pxa_memory_release(vision.bytes);
        xSemaphoreTake(vision.mutex, portMAX_DELAY);
        vision.bytes = nullptr; vision.used = vision.offset = vision.capacity = 0;
        vision.prepared = vision.allocated = vision.closing = false;
        Unlock(); return false;
    }
    if (!vision.live || (vision.status != PXA_STATUS_OK && vision.status != PXA_STATUS_WOULD_BLOCK) || vision.used) {
        Unlock(); return false;
    }
    const bool prepare = !vision.prepared;
    const auto format = vision.format;
    Unlock();
    if (prepare) {
        vision.capacity = (format == PXA_VISION_JPEG ? PXA_VISION_MAX_JPEG : PXA_VISION_MAX_JSON) + PXA_VISION_HEADER_BYTES;
        const auto* allocator = pxa_esp_device_resource_allocator(PXA_MEMORY_EXTERNAL, PXA_MEMORY_FRAME);
        vision.bytes = static_cast<uint8_t*>(pxa_memory_allocate(allocator, vision.capacity));
        vision.allocated = true; // finish also rolls back partial preparation.
        const auto status = vision.bytes ? vision.driver.prepare(vision.driver.context, format) : PXA_STATUS_RESOURCE_LIMIT;
        xSemaphoreTake(vision.mutex, portMAX_DELAY);
        vision.prepared = status == PXA_STATUS_OK;
        vision.status = status;
        const bool again = vision.closing || vision.prepared;
        Unlock(); return again;
    }
    size_t size = 0;
    auto status = vision.driver.read(vision.driver.context, vision.bytes + PXA_VISION_HEADER_BYTES,
                                    vision.capacity - PXA_VISION_HEADER_BYTES, &size);
    const pxa_vision_frame_info_t info{format, static_cast<uint32_t>(size), ++vision.sequence,
                                     static_cast<uint64_t>(esp_timer_get_time())};
    if (status == PXA_STATUS_OK && (size > UINT32_MAX || !pxa_vision_encode(vision.bytes, &info)))
        status = PXA_STATUS_PROTOCOL_ERROR;
    // Drivers must return a frame or a terminal error after their deadline;
    // a WOULD_BLOCK result here is not a busy-loop retry contract.
    if (status == PXA_STATUS_WOULD_BLOCK) status = PXA_STATUS_UNAVAILABLE;
    if (status != PXA_STATUS_OK) ESP_LOGW("PxaVision", "Capture failed: %ld", long(status));
    xSemaphoreTake(vision.mutex, portMAX_DELAY);
    const bool closing = vision.closing;
    if (!closing) {
        vision.status = status;
        if (status == PXA_STATUS_OK) { vision.used = PXA_VISION_HEADER_BYTES + size; vision.offset = 0; }
    }
    Unlock(); return closing;
}
void Worker(void*) {
    for (;;) {
        if (Pump()) continue;
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
}
}
bool pxa_board_vision_port_initialize(const PxaBoardVisionDriver* driver) {
    if (!driver || (!driver->jpeg && !driver->inference) || !driver->prepare || !driver->read || !driver->finish || vision.task)
        return false;
    vision.driver = *driver;
    vision.mutex = xSemaphoreCreateMutexStatic(&vision.mutex_storage);
    if (!vision.mutex) return false;
    vision.task = xTaskCreateStatic(Worker, "pxa_vision", sizeof(vision.stack), nullptr, 2, vision.stack, &vision.task_storage);
    if (!vision.task) return false;
    const pxa_peripherals_backend_t backend = {sizeof(pxa_peripherals_backend_t), nullptr,
        List, Module, Open, Io, Start, Poll, Cancel, Close, Ready};
    return pxa_board_add_peripheral_backend(&backend);
}
