// SPDX-License-Identifier: Apache-2.0
#include <pxa_board_common/rgb_port.h>
#include <pxa_board_common/peripheral_backends.h>
#include <pxa/pxa_worker_notifications.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <cstring>

namespace {
struct State {
    bool live = false, closing = false, dirty = false;
    unsigned color = 0;
    void* context = nullptr;
    bool (*apply)(void*, unsigned, unsigned, unsigned) = nullptr;
    bool (*apply_binary)(void*, bool) = nullptr;
    bool binary = false;
    SemaphoreHandle_t mutex = nullptr;
    TaskHandle_t task = nullptr;
    StaticSemaphore_t mutex_storage;
    StaticTask_t task_storage;
    StackType_t stack[3072 / sizeof(StackType_t)];
    PxaWorkerNotifications notifications;
} state;
bool Lock() { return xSemaphoreTake(state.mutex, 0) == pdTRUE; }
void Unlock() { xSemaphoreGive(state.mutex); }
void Notify() { (void)state.notifications.Notify(state.task); }
pxa_status_t List(void*, pxa_peripheral_port_t* ports, size_t capacity, size_t* count) {
    *count = 1;
    if (!capacity) return PXA_STATUS_LIMIT_EXCEEDED;
    ports[0] = {};
    ports[0].id = 1; ports[0].kind = state.binary ? PXA_PERIPHERALS_GPIO : PXA_PERIPHERALS_RGB;
    ports[0].flags = PXA_PERIPHERALS_WRITE;
    std::strcpy(ports[0].name, state.binary ? "indicator.status" : "indicator.rgb");
    return PXA_STATUS_OK;
}
pxa_status_t Module(void*, uint16_t, pxa_peripheral_module_t*) { return PXA_STATUS_NOT_FOUND; }
pxa_status_t Open(void*, uint64_t owner, uint16_t port, uint16_t mode,
                  uint32_t parameter, uint16_t address, void** output) {
    *output = nullptr;
    if (!owner || port != 1 || mode != (state.binary ? PXA_PERIPHERALS_MODE_OUTPUT : PXA_PERIPHERALS_MODE_RGB) ||
        address || parameter > (state.binary ? 1u : 0xffffffu)) return PXA_STATUS_INVALID_ARGUMENT;
    if (!Lock()) return PXA_STATUS_WOULD_BLOCK;
    if (state.live || state.closing) { Unlock(); return PXA_STATUS_BUSY; }
    state.live = true; state.color = state.binary ? parameter << 16 : parameter; state.dirty = true;
    *output = &state;
    Unlock(); Notify(); return PXA_STATUS_OK;
}
int32_t Io(void*, void* device, uint32_t operation, uint8_t* bytes, size_t size) {
    if (device != &state) return PXA_STATUS_INVALID_ARGUMENT;
    if (operation != PXA_IO_WRITE) return PXA_STATUS_UNSUPPORTED;
    if (!bytes || size != (state.binary ? 1u : 3u) || (state.binary && bytes[0] > 1)) return PXA_STATUS_INVALID_ARGUMENT;
    if (!Lock()) return PXA_STATUS_WOULD_BLOCK;
    if (!state.live || state.closing) { Unlock(); return PXA_STATUS_UNAVAILABLE; }
    state.color = state.binary ? unsigned(bytes[0]) << 16 :
        (unsigned(bytes[0]) << 16) | (unsigned(bytes[1]) << 8) | bytes[2];
    state.dirty = true;
    Unlock(); Notify(); return static_cast<int32_t>(size);
}
pxa_status_t Start(void*, void*, uint64_t, const uint8_t*, uint16_t, uint16_t) { return PXA_STATUS_UNSUPPORTED; }
pxa_status_t Poll(void*, void*, uint64_t, uint8_t*, size_t*) { return PXA_STATUS_UNSUPPORTED; }
void Cancel(void*, void*, uint64_t) {}
void Close(void*, void* device) {
    if (device != &state) return;
    // The worker never holds this lock during device I/O. Host close must not
    // lose cleanup when a write is being copied, so take the short mutex.
    xSemaphoreTake(state.mutex, portMAX_DELAY);
    state.live = false; state.closing = true; state.color = 0; state.dirty = true;
    Unlock(); Notify();
}
bool ApplyPending() {
        xSemaphoreTake(state.mutex, portMAX_DELAY);
        const bool dirty = state.dirty;
        const bool closing = state.closing;
        const unsigned color = state.color;
        state.dirty = false;
        Unlock();
        if (dirty) {
            const bool applied = state.apply(state.context, (color >> 16) & 255, (color >> 8) & 255, color & 255);
            if (!applied)
                ESP_LOGW("PxaRgb", "Indicator update failed");
            xSemaphoreTake(state.mutex, portMAX_DELAY);
            // No new owner can open until this black write has completed.
            if (closing && applied) state.closing = false;
            if (closing && !applied) state.dirty = true;
            const bool again = state.dirty;
            Unlock();
            if (closing && !applied) vTaskDelay(pdMS_TO_TICKS(20));
            return again;
        }
        return false;
}
void Worker(void*) {
    for (;;) {
        if (ApplyPending()) continue;
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
}
}

namespace {
bool Initialize(void* context, bool (*apply)(void*, unsigned, unsigned, unsigned), bool binary) {
    if (!apply || state.task) return false;
    state.binary = binary;
    state.context = context; state.apply = apply;
    state.mutex = xSemaphoreCreateMutexStatic(&state.mutex_storage);
    if (!state.mutex) return false;
    state.task = xTaskCreateStatic(Worker, "pxa_rgb", sizeof(state.stack), nullptr, 2,
                                 state.stack, &state.task_storage);
    if (!state.task) return false;
    const pxa_peripherals_backend_t backend = {
        sizeof(pxa_peripherals_backend_t), nullptr, List, Module, Open, Io,
        Start, Poll, Cancel, Close, nullptr,
    };
    return pxa_board_add_peripheral_backend(&backend);
}
}
bool pxa_board_rgb_port_initialize(void* context, bool (*apply)(void*, unsigned, unsigned, unsigned)) {
    return Initialize(context, apply, false);
}
bool pxa_board_binary_indicator_initialize(void* context, bool (*apply)(void*, bool)) {
    if (!apply || state.task) return false;
    state.apply_binary = apply;
    return Initialize(context, [](void* c, unsigned r, unsigned, unsigned) { return state.apply_binary(c, r != 0); }, true);
}
