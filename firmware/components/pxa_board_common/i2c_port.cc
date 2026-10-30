// SPDX-License-Identifier: Apache-2.0
#include <pxa_board_common/i2c_port.h>
#include <pxa_board_common/peripheral_backends.h>
#include <pxa/pxa_worker_notifications.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <cstring>

namespace {
struct Device {
    bool live = false, closing = false, opening = false, prepared = false;
    bool working = false, pending = false, complete = false, cancelled = false;
    uint16_t address = 0, tx_size = 0, rx_size = 0;
    uint32_t hz = 0;
    uint64_t cookie = 0;
    pxa_status_t result = PXA_STATUS_WOULD_BLOCK;
    i2c_master_dev_handle_t handle = nullptr;
    uint8_t tx[256], rx[256];
};
struct State {
    i2c_master_bus_handle_t bus = nullptr;
    pxa_peripheral_port_t port{};
    uint8_t denied[16]; size_t denied_count = 0;
    Device devices[4];
    StaticSemaphore_t mutex_storage;
    SemaphoreHandle_t mutex = nullptr;
    StaticTask_t task_storage;
    StackType_t stack[4096 / sizeof(StackType_t)];
    TaskHandle_t task = nullptr;
    PxaWorkerNotifications notifications;
} state;
bool Lock() { return xSemaphoreTake(state.mutex, 0) == pdTRUE; }
void Unlock() { xSemaphoreGive(state.mutex); }
void Notify() { (void)state.notifications.Notify(state.task); }
Device* Get(void* native) {
    for (auto& d : state.devices) if (&d == native) return &d;
    return nullptr;
}
pxa_status_t List(void*, pxa_peripheral_port_t* output, size_t capacity, size_t* count) {
    *count = 1;
    if (!capacity) return PXA_STATUS_LIMIT_EXCEEDED;
    *output = state.port; return PXA_STATUS_OK;
}
pxa_status_t Module(void*, uint16_t, pxa_peripheral_module_t*) { return PXA_STATUS_NOT_FOUND; }
pxa_status_t Open(void*, uint64_t owner, uint16_t port, uint16_t mode, uint32_t parameter, uint16_t address, void** output) {
    *output = nullptr;
    if (!owner || port != state.port.id || mode != PXA_PERIPHERALS_MODE_I2C ||
        parameter < 10000 || parameter > 400000 || address < 8 || address > 0x77) return PXA_STATUS_INVALID_ARGUMENT;
    for (size_t i = 0; i < state.denied_count; ++i) if (address == state.denied[i]) return PXA_STATUS_DENIED;
    if (!Lock()) return PXA_STATUS_WOULD_BLOCK;
    Device* free = nullptr;
    for (auto& d : state.devices) {
        if ((d.live || d.closing) && d.address == address) { Unlock(); return PXA_STATUS_BUSY; }
        if (!d.live && !d.closing && !d.working && !d.handle && !free) free = &d;
    }
    if (!free) { Unlock(); return PXA_STATUS_RESOURCE_LIMIT; }
    *free = {}; free->live = free->opening = true;
    free->address = address; free->hz = parameter; *output = free;
    Unlock(); Notify(); return PXA_STATUS_OK;
}
pxa_status_t Ready(void*, void* native) {
    auto* d = Get(native);
    if (!d) return PXA_STATUS_INVALID_ARGUMENT;
    if (!Lock()) return PXA_STATUS_WOULD_BLOCK;
    const auto result = !d->live || d->closing ? PXA_STATUS_UNAVAILABLE :
        (d->prepared ? d->result : PXA_STATUS_WOULD_BLOCK);
    Unlock(); return result;
}
int32_t Io(void*, void*, uint32_t, uint8_t*, size_t) { return PXA_STATUS_UNSUPPORTED; }
pxa_status_t Start(void*, void* native, uint64_t cookie, const uint8_t* tx, uint16_t n, uint16_t rx) {
    auto* d = Get(native);
    if (!d || !cookie || n > 256 || rx > 256 || (!n && !rx) || (n && !tx)) return PXA_STATUS_INVALID_ARGUMENT;
    if (!Lock()) return PXA_STATUS_WOULD_BLOCK;
    if (!d->live || d->closing) { Unlock(); return PXA_STATUS_UNAVAILABLE; }
    if (!d->prepared) { Unlock(); return PXA_STATUS_WOULD_BLOCK; }
    if (d->result != PXA_STATUS_OK && !d->handle) { auto result = d->result; Unlock(); return result; }
    if (d->working || d->pending || d->complete) { Unlock(); return PXA_STATUS_BUSY; }
    if (n) std::memcpy(d->tx, tx, n);
    d->tx_size = n; d->rx_size = rx; d->cookie = cookie;
    d->pending = true; d->cancelled = false;
    Unlock(); Notify(); return PXA_STATUS_OK;
}
pxa_status_t Poll(void*, void* native, uint64_t cookie, uint8_t* rx, size_t* size) {
    auto* d = Get(native);
    if (!d || !size) return PXA_STATUS_INVALID_ARGUMENT;
    if (!Lock()) return PXA_STATUS_WOULD_BLOCK;
    if (!d->live || d->closing || d->cookie != cookie) { Unlock(); return PXA_STATUS_CANCELLED; }
    if (!d->complete) { Unlock(); return PXA_STATUS_WOULD_BLOCK; }
    const auto result = d->result;
    if (result == PXA_STATUS_OK) {
        if (*size < d->rx_size || (d->rx_size && !rx)) { Unlock(); return PXA_STATUS_INVALID_ARGUMENT; }
        if (d->rx_size) std::memcpy(rx, d->rx, d->rx_size);
        *size = d->rx_size;
    }
    d->complete = false; Unlock(); return result;
}
void Cancel(void*, void* native, uint64_t cookie) {
    auto* d = Get(native); if (!d) return;
    xSemaphoreTake(state.mutex, portMAX_DELAY);
    if (d->cookie == cookie) { d->cancelled = true; d->pending = d->complete = false; }
    Unlock();
}
void Close(void*, void* native) {
    auto* d = Get(native); if (!d) return;
    xSemaphoreTake(state.mutex, portMAX_DELAY);
    d->live = false; d->closing = d->cancelled = true; d->pending = d->complete = false;
    Unlock(); Notify();
}
bool Process(Device& d) {
    xSemaphoreTake(state.mutex, portMAX_DELAY);
    const bool closing = d.closing, opening = d.opening && !closing, transfer = d.pending && !closing;
    if (!closing && !opening && !transfer) { Unlock(); return false; }
    d.working = true; d.pending = false;
    Unlock();
    int error = ESP_OK;
    if (closing) {
        if (d.handle) error = i2c_master_bus_rm_device(d.handle);
    } else if (opening) {
        i2c_device_config_t config{};
        config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        config.device_address = d.address; config.scl_speed_hz = d.hz;
        error = i2c_master_bus_add_device(state.bus, &config, &d.handle);
    } else if (d.tx_size && d.rx_size) {
        error = i2c_master_transmit_receive(d.handle, d.tx, d.tx_size, d.rx, d.rx_size, 100);
    } else if (d.tx_size) {
        error = i2c_master_transmit(d.handle, d.tx, d.tx_size, 100);
    } else {
        error = i2c_master_receive(d.handle, d.rx, d.rx_size, 100);
    }
    xSemaphoreTake(state.mutex, portMAX_DELAY);
    d.working = false;
    if (closing) {
        if (error == ESP_OK) { d.handle = nullptr; d.closing = d.opening = false; }
    } else if (opening) {
        d.opening = false; d.prepared = true;
        d.result = error == ESP_OK && d.handle ? PXA_STATUS_OK : PXA_STATUS_IO_ERROR;
    } else if (!d.closing && !d.cancelled) {
        d.result = error == ESP_OK ? PXA_STATUS_OK : PXA_STATUS_IO_ERROR;
        d.complete = true;
    }
    const bool again = d.closing;
    Unlock();
    if (closing && error != ESP_OK) vTaskDelay(pdMS_TO_TICKS(20));
    return again;
}
void Worker(void*) {
    for (;;) {
        bool again = false;
        for (auto& d : state.devices) again |= Process(d);
        if (!again) ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
}
}
bool pxa_board_i2c_port_initialize(i2c_master_bus_handle_t bus, uint16_t port,
    const char* scope, const uint8_t* denied, size_t denied_count) {
    if (!bus || !port || !scope || !*scope || std::strlen(scope) >= sizeof(state.port.name) ||
        denied_count > sizeof(state.denied) || (denied_count && !denied) || state.task) return false;
    state.bus = bus; state.port.id = port; state.port.kind = PXA_PERIPHERALS_I2C;
    state.port.flags = PXA_PERIPHERALS_READ | PXA_PERIPHERALS_WRITE;
    std::strcpy(state.port.name, scope);
    if (denied_count) std::memcpy(state.denied, denied, denied_count);
    state.denied_count = denied_count;
    state.mutex = xSemaphoreCreateMutexStatic(&state.mutex_storage);
    if (!state.mutex) return false;
    state.task = xTaskCreateStatic(Worker, "pxa_ext_i2c", sizeof(state.stack), nullptr, 2, state.stack, &state.task_storage);
    if (!state.task) return false;
    const pxa_peripherals_backend_t backend = {sizeof(pxa_peripherals_backend_t), nullptr,
        List, Module, Open, Io, Start, Poll, Cancel, Close, Ready};
    return pxa_board_add_peripheral_backend(&backend);
}
