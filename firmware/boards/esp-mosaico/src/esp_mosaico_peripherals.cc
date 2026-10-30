#include "esp_mosaico_peripherals.h"
#include "esp_mosaico_module_descriptor.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <driver/gpio.h>
#include <driver/uart.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <pxa/pxa_peripherals.h>
#include <pxa/pxa_worker_notifications.h>
#include <pxa/pxa_esp_resource_memory.h>

namespace {
constexpr size_t kDevices = 16, kStackBytes = 4096;
constexpr auto kUart = UART_NUM_1;
constexpr int kSelector[2] = {14, 39};
constexpr int kLeftPins[] = {53, 48, 13, 12, 14, 4, 55, 19, 18, 17, 16, 15};
constexpr int kRightPins[] = {46, 47, 11, 10, 39, 5, 38};
struct Device {
    bool live = false, pending = false, complete = false, opening = false, prepared = false;
    uint16_t port = 0, mode = 0, address = 0, tx_size = 0, rx_size = 0;
    int pin = -1, group = -1;
    uint32_t parameter = 0;
    bool inverted = false, uart_installed = false, touched = false;
    uint64_t owner = 0, cookie = 0;
    std::atomic<bool> closing{false}, cancelled{false};
    i2c_master_dev_handle_t i2c = nullptr;
    uint8_t* tx = nullptr;
    uint8_t* rx = nullptr;
    pxa_status_t result = PXA_STATUS_OK, open_result = PXA_STATUS_WOULD_BLOCK;
};
struct Slot {
    pxa_peripheral_module_t info = {};
    uint64_t owner = 0;
    unsigned references = 0, debounce = 0;
    int candidate = -1;
    bool scanning = false, restoring = false;
    uint32_t last_descriptor_ms = 0, last_device_attempt_ms = 0;
    bool device_attempted = false;
    i2c_master_dev_handle_t eeprom = nullptr;
};
std::array<Device, kDevices> g_devices;
Slot g_slots[2];
pxa_peripheral_port_t g_ports[32];
size_t g_port_count = 0;
i2c_master_bus_handle_t g_bus = nullptr;
SemaphoreHandle_t g_mutex = nullptr;
StaticSemaphore_t g_mutex_storage;
StaticTask_t g_task_storage;
StackType_t g_stack[kStackBytes / sizeof(StackType_t)];
TaskHandle_t g_task = nullptr;
PxaWorkerNotifications g_notifications;
std::atomic<bool> g_shutdown{false}, g_stopped{false};
bool g_initialized = false;
bool g_usb_ready = false;
uint16_t g_revision = 0;
bool Lock() { return xSemaphoreTake(g_mutex, 0) == pdTRUE; }
void Unlock() { xSemaphoreGive(g_mutex); }
void AddPort(uint16_t id, uint16_t kind, uint32_t flags, const char* name) {
    auto& p = g_ports[g_port_count++]; p.id = id; p.kind = kind; p.flags = flags;
    std::snprintf(p.name, sizeof(p.name), "%s", name);
}
bool ConfigureSelector(unsigned slot) {
    gpio_config_t config = {};
    config.pin_bit_mask = 1ULL << kSelector[slot]; config.mode = GPIO_MODE_OUTPUT;
    return gpio_set_level(gpio_num_t(kSelector[slot]), slot ? 1 : 0) == ESP_OK && gpio_config(&config) == ESP_OK;
}
int Pin(uint16_t id) {
    if (id >= 16 && id < 16 + std::size(kLeftPins)) return kLeftPins[id-16];
    if (id >= 32 && id < 32 + std::size(kRightPins)) return kRightPins[id-32];
    return id == 48 && g_revision == 0x0100 ? 3 : -1;
}
int Group(uint16_t id) {
    if (id == 1 || (id >= 16 && id < 28)) return 0;
    if (id == 2 || id == 4 || (id >= 32 && id < 39)) return 1;
    return -1;
}
const pxa_peripheral_port_t* Port(uint16_t id) {
    for (size_t i = 0; i < g_port_count; ++i) if (g_ports[i].id == id) return &g_ports[i];
    return nullptr;
}
pxa_status_t List(void*, pxa_peripheral_port_t* output, size_t capacity, size_t* count) {
    if (capacity < g_port_count) return PXA_STATUS_LIMIT_EXCEEDED;
    *count = g_port_count; std::copy_n(g_ports, g_port_count, output); return PXA_STATUS_OK;
}
pxa_status_t Module(void*, uint16_t port, pxa_peripheral_module_t* output) {
    if (port < 1 || port > 2) return PXA_STATUS_NOT_FOUND;
    if (!Lock()) return PXA_STATUS_WOULD_BLOCK;
    *output = g_slots[port-1].info;
    if (g_slots[port-1].references) output->flags |= 4;
    Unlock(); return PXA_STATUS_OK;
}
pxa_status_t Open(void*, uint64_t owner, uint16_t port_id, uint16_t mode,
    uint32_t parameter, uint16_t address, void** output) {
    *output = nullptr;
    if (g_shutdown.load()) return PXA_STATUS_UNAVAILABLE;
    const auto* port = Port(port_id);
    if (!port || (port->flags & PXA_PERIPHERALS_RESERVED)) return PXA_STATUS_UNAVAILABLE;
    if (!owner) return PXA_STATUS_INVALID_ARGUMENT;
    if ((port->kind == PXA_PERIPHERALS_GPIO && mode != PXA_PERIPHERALS_MODE_INPUT && mode != PXA_PERIPHERALS_MODE_OUTPUT) ||
        (port->kind == PXA_PERIPHERALS_I2C && mode != PXA_PERIPHERALS_MODE_I2C) ||
        (port->kind == PXA_PERIPHERALS_UART && mode != PXA_PERIPHERALS_MODE_UART) ||
        (port->kind == PXA_PERIPHERALS_SLOT && mode != PXA_PERIPHERALS_MODE_SLOT)) return PXA_STATUS_INVALID_ARGUMENT;
    if ((mode == PXA_PERIPHERALS_MODE_INPUT && parameter) ||
        (mode == PXA_PERIPHERALS_MODE_OUTPUT && parameter > 1) ||
        (mode == PXA_PERIPHERALS_MODE_SLOT && parameter) ||
        (mode != PXA_PERIPHERALS_MODE_I2C && address) ||
        (port_id == 48 && mode != PXA_PERIPHERALS_MODE_OUTPUT)) return PXA_STATUS_INVALID_ARGUMENT;
    if (mode == PXA_PERIPHERALS_MODE_I2C &&
        (!g_bus || parameter < 10000 || parameter > 400000 || address < 8 || address > 0x77 ||
         address == 0x50 || address == 0x51)) return PXA_STATUS_DENIED;
    if (mode == PXA_PERIPHERALS_MODE_I2C && g_revision == 0x0100 &&
        (address == 0x11 || address == 0x12 || address == 0x19 || address == 0x55 ||
         address == 0x5a || address == 0x69)) return PXA_STATUS_DENIED;
    if (mode == PXA_PERIPHERALS_MODE_UART && (parameter < 1200 || parameter > 921600 || !g_usb_ready))
        return PXA_STATUS_INVALID_ARGUMENT;
    if (!Lock()) return PXA_STATUS_WOULD_BLOCK;
    if (g_shutdown.load()) { Unlock(); return PXA_STATUS_UNAVAILABLE; }
    Device* device = nullptr;
    for (auto& d : g_devices) {
        if (d.live && (d.port == port_id && mode != PXA_PERIPHERALS_MODE_I2C)) { Unlock(); return PXA_STATUS_BUSY; }
        if (d.live && mode == PXA_PERIPHERALS_MODE_I2C && d.mode == mode && d.address == address) {
            Unlock(); return PXA_STATUS_BUSY;
        }
        if (!d.live && !d.closing.load() && !device) device = &d;
    }
    const int group = Group(port_id);
    if (group >= 0 && (g_slots[group].scanning || g_slots[group].restoring ||
        (g_slots[group].references && g_slots[group].owner != owner))) { Unlock(); return PXA_STATUS_BUSY; }
    if (!device) { Unlock(); return PXA_STATUS_RESOURCE_LIMIT; }
    const int pin = Pin(port_id);
    if (port->kind == PXA_PERIPHERALS_GPIO && pin < 0) { Unlock(); return PXA_STATUS_NOT_FOUND; }
    // Host callbacks only reserve metadata; the worker owns all driver setup.
    // Reservations also protect queued opens from conflicting slot probes.
    device->live = true; device->pending = device->complete = false;
    device->cancelled = false; device->closing = false;
    device->i2c = nullptr; device->uart_installed = false;
    device->port = port_id; device->mode = mode; device->address = address; device->parameter = parameter;
    device->pin = pin; device->inverted = port_id == 48;
    device->owner = owner; device->group = group;
    if (group >= 0) { g_slots[group].owner = owner; ++g_slots[group].references; }
    device->opening = true; device->prepared = device->touched = false;
    device->open_result = PXA_STATUS_WOULD_BLOCK;
    *output = device; Unlock(); (void)g_notifications.Notify(g_task); return PXA_STATUS_OK;
}
pxa_status_t Ready(void*, void* native) {
    auto* device = static_cast<Device*>(native);
    if (!Lock()) return PXA_STATUS_WOULD_BLOCK;
    const auto status = g_shutdown.load() || !device->live || device->closing.load() ? PXA_STATUS_UNAVAILABLE :
        (device->prepared ? device->open_result : PXA_STATUS_WOULD_BLOCK);
    Unlock(); return status;
}
void Prepare(Device& d) {
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    if (!d.live || !d.opening || d.closing.load() || g_shutdown.load()) { Unlock(); return; }
    d.opening = false;
    Unlock();
    auto* device = &d;
    const auto mode = device->mode;
    const auto parameter = device->parameter;
    const auto address = device->address;
    const auto pin = device->pin;
    // These fields are worker-owned until prepared is published under g_mutex.
    int error = ESP_OK;
    if (mode == PXA_PERIPHERALS_MODE_I2C) {
        device->touched = true;
        i2c_device_config_t config = {};
        config.dev_addr_length = I2C_ADDR_BIT_LEN_7; config.device_address = address; config.scl_speed_hz = parameter;
        error = i2c_master_bus_add_device(g_bus, &config, &device->i2c);
    } else if (mode == PXA_PERIPHERALS_MODE_UART) {
        if (uart_is_driver_installed(kUart)) {
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            device->open_result = PXA_STATUS_BUSY; device->prepared = true;
            Unlock(); return;
        }
        device->touched = true;
        uart_config_t config = {};
        config.baud_rate = parameter; config.data_bits = UART_DATA_8_BITS; config.parity = UART_PARITY_DISABLE;
        config.stop_bits = UART_STOP_BITS_1; config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE; config.source_clk = UART_SCLK_DEFAULT;
        error = uart_param_config(kUart, &config);
        if (error == ESP_OK) error = uart_set_pin(kUart, 58, 59, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        if (error == ESP_OK) error = uart_driver_install(kUart, 1024, 0, 0, nullptr, 0);
        device->uart_installed = uart_is_driver_installed(kUart);
    } else if (mode == PXA_PERIPHERALS_MODE_INPUT || mode == PXA_PERIPHERALS_MODE_OUTPUT) {
        device->touched = true;
        gpio_config_t config = {};
        config.pin_bit_mask = 1ULL << pin;
        config.mode = mode == PXA_PERIPHERALS_MODE_INPUT ? GPIO_MODE_INPUT : GPIO_MODE_INPUT_OUTPUT;
        if (mode == PXA_PERIPHERALS_MODE_OUTPUT) error = gpio_set_level(gpio_num_t(pin), parameter ^ device->inverted);
        if (error == ESP_OK) error = gpio_config(&config);
    }
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    device->open_result = error == ESP_OK ? PXA_STATUS_OK : PXA_STATUS_IO_ERROR;
    device->prepared = true;
    Unlock();
}
int32_t Io(void*, void* native, uint32_t operation, uint8_t* data, size_t size) {
    auto* d = static_cast<Device*>(native);
    if (!Lock()) return PXA_STATUS_WOULD_BLOCK;
    if (g_shutdown.load() || !d->live || d->closing.load()) { Unlock(); return PXA_STATUS_UNAVAILABLE; }
    if (!d->prepared || d->open_result != PXA_STATUS_OK) {
        const auto status = d->prepared ? d->open_result : PXA_STATUS_WOULD_BLOCK;
        Unlock(); return status;
    }
    int32_t result = PXA_STATUS_UNSUPPORTED;
    if (d->mode == PXA_PERIPHERALS_MODE_INPUT || d->mode == PXA_PERIPHERALS_MODE_OUTPUT) {
        if (size != 1) result = PXA_STATUS_INVALID_ARGUMENT;
        else if (operation == PXA_IO_READ) { data[0] = gpio_get_level(gpio_num_t(d->pin)) ^ d->inverted; result = 1; }
        else if (operation == PXA_IO_WRITE && d->mode == PXA_PERIPHERALS_MODE_OUTPUT) {
            result = data[0] > 1 ? PXA_STATUS_INVALID_ARGUMENT :
                (gpio_set_level(gpio_num_t(d->pin), data[0] ^ d->inverted) == ESP_OK ? 1 : PXA_STATUS_IO_ERROR);
        }
    } else if (d->mode == PXA_PERIPHERALS_MODE_UART) {
        const int count = operation == PXA_IO_READ ? uart_read_bytes(kUart, data, size, 0) :
            uart_tx_chars(kUart, reinterpret_cast<const char*>(data), size);
        result = count < 0 ? PXA_STATUS_IO_ERROR : (count ? count : PXA_STATUS_WOULD_BLOCK);
    }
    Unlock(); return result;
}
pxa_status_t Start(void*, void* native, uint64_t cookie, const uint8_t* tx, uint16_t tx_size, uint16_t rx_size) {
    auto* d = static_cast<Device*>(native);
    if (!Lock()) return PXA_STATUS_WOULD_BLOCK;
    if (g_shutdown.load() || !d->live || d->closing.load()) { Unlock(); return PXA_STATUS_UNAVAILABLE; }
    if (!d->prepared || d->open_result != PXA_STATUS_OK) {
        const auto status = d->prepared ? d->open_result : PXA_STATUS_WOULD_BLOCK;
        Unlock(); return status;
    }
    if (d->mode != PXA_PERIPHERALS_MODE_I2C) { Unlock(); return PXA_STATUS_UNSUPPORTED; }
    if (d->pending || d->complete) { Unlock(); return PXA_STATUS_BUSY; }
    d->cookie = cookie; d->tx_size = tx_size; d->rx_size = rx_size;
    std::memcpy(d->tx, tx, tx_size); d->cancelled = false; d->pending = true;
    Unlock(); (void)g_notifications.Notify(g_task); return PXA_STATUS_OK;
}
pxa_status_t Poll(void*, void* native, uint64_t cookie, uint8_t* output, size_t* bytes) {
    auto* d = static_cast<Device*>(native);
    if (!Lock()) return PXA_STATUS_WOULD_BLOCK;
    if (!d->live || d->cookie != cookie || d->closing.load()) { Unlock(); return PXA_STATUS_CANCELLED; }
    if (!d->complete) { Unlock(); return PXA_STATUS_WOULD_BLOCK; }
    if (*bytes < d->rx_size) { Unlock(); return PXA_STATUS_LIMIT_EXCEEDED; }
    const auto result = d->result;
    *bytes = result == PXA_STATUS_OK ? d->rx_size : 0;
    if (*bytes) std::memcpy(output, d->rx, *bytes);
    d->complete = false;
    Unlock(); return result;
}
void Cancel(void*, void* native, uint64_t cookie) {
    auto* d = static_cast<Device*>(native);
    // Cookies are written only by serialized Host calls, never by the worker.
    if (d->cookie == cookie) d->cancelled = true;
    (void)g_notifications.Notify(g_task);
}
void Close(void*, void* native) {
    static_cast<Device*>(native)->closing = true;
    (void)g_notifications.Notify(g_task);
}
void Cleanup(Device& d) {
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    const bool cleanup = d.closing.load() || (d.live && d.prepared && d.open_result != PXA_STATUS_OK);
    Unlock();
    if (!cleanup) return;
    int error = ESP_OK;
    if (d.touched) {
        if (d.mode == PXA_PERIPHERALS_MODE_I2C && d.i2c) error = i2c_master_bus_rm_device(d.i2c);
        else if (d.mode == PXA_PERIPHERALS_MODE_UART) {
            if (d.uart_installed) {
                error = uart_driver_delete(kUart);
                if (error == ESP_OK) d.uart_installed = false;
            }
            if (error == ESP_OK) {
                const auto tx = gpio_reset_pin(GPIO_NUM_58);
                const auto rx = gpio_reset_pin(GPIO_NUM_59);
                error = tx == ESP_OK ? rx : tx;
            }
        } else if (d.pin >= 0) {
            error = gpio_set_level(gpio_num_t(d.pin), d.inverted ? 1 : 0);
            if (!d.inverted) error = gpio_reset_pin(gpio_num_t(d.pin));
        }
        if (error != ESP_OK) return; // Keep ownership and retry; never free a live device.
        d.i2c = nullptr; d.touched = false;
    }
    // Failed setup rolls back hardware immediately, but its record remains
    // until Host observes the error and closes its authority-bound handle.
    if (!d.closing.load()) return;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    if (d.group >= 0 && --g_slots[d.group].references == 0) {
        g_slots[d.group].owner = 0; g_slots[d.group].restoring = true;
    }
    d.i2c = nullptr; d.opening = d.prepared = d.pending = d.complete = d.live = false; d.closing = false;
    Unlock();
}
void Transfer(Device& d) {
    uint8_t tx[256], rx[256];
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    if (!d.live || !d.pending || d.closing.load()) { Unlock(); return; }
    const uint16_t tx_size = d.tx_size, rx_size = d.rx_size;
    // Account for wire time at 10 kHz as well as maximum-size combined I/O.
    // A fixed 20 ms timeout would reject valid long/slow transactions.
    const int timeout_ms = 20 + ((uint32_t(tx_size) + rx_size + 4) * 9000 + d.parameter - 1) / d.parameter;
    std::memcpy(tx, d.tx, tx_size);
    const bool cancelled = d.cancelled.load();
    Unlock();
    int error = ESP_OK;
    if (!cancelled) {
        if (tx_size && rx_size) error = i2c_master_transmit_receive(d.i2c, tx, tx_size, rx, rx_size, timeout_ms);
        else if (rx_size) error = i2c_master_receive(d.i2c, rx, rx_size, timeout_ms);
        else error = i2c_master_transmit(d.i2c, tx, tx_size, timeout_ms);
    }
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    d.result = cancelled || d.cancelled.load() || d.closing.load() ? PXA_STATUS_CANCELLED :
        (error == ESP_OK ? PXA_STATUS_OK : PXA_STATUS_IO_ERROR);
    if (d.result == PXA_STATUS_OK && rx_size) std::memcpy(d.rx, rx, rx_size);
    d.pending = false; d.complete = true;
    Unlock();
}
void ScanError(Slot& slot) {
    if (!(slot.info.flags & 16)) ++slot.info.generation;
    slot.info.flags |= 16;
    slot.candidate = -1; slot.debounce = 0; slot.scanning = false;
}
void Scan(unsigned index, uint32_t now) {
    auto& slot = g_slots[index];
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    if (!g_bus || slot.references || slot.restoring) { Unlock(); return; }
    if (!slot.eeprom && slot.device_attempted && now - slot.last_device_attempt_ms < 2000) {
        Unlock(); return;
    }
    slot.scanning = true;
    const bool create = !slot.eeprom;
    if (create) { slot.device_attempted = true; slot.last_device_attempt_ms = now; }
    Unlock();
    if (create) {
        i2c_device_config_t config = {};
        config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        config.device_address = 0x50 + index; config.scl_speed_hz = 100000;
        i2c_master_dev_handle_t eeprom = nullptr;
        const auto error = i2c_master_bus_add_device(g_bus, &config, &eeprom);
        xSemaphoreTake(g_mutex, portMAX_DELAY);
        if (error != ESP_OK || !eeprom) { ScanError(slot); Unlock(); return; }
        slot.eeprom = eeprom;
        Unlock();
    }
    const auto probe = i2c_master_probe(g_bus, 0x50 + index, 5);
    const bool present = probe == ESP_OK;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    if (probe != ESP_OK && probe != ESP_ERR_NOT_FOUND) { ScanError(slot); Unlock(); return; }
    const bool stabilizing = slot.candidate == int(present) && slot.debounce == 2;
    if (slot.candidate != int(present)) { slot.candidate = present; slot.debounce = 1; }
    else if (slot.debounce < 3) ++slot.debounce;
    const bool changed = slot.debounce == 3 && ((slot.info.flags & 8) || bool(slot.info.flags & 1) != present ||
        (!present && (slot.info.flags & 16)));
    if (changed) {
        const uint32_t generation = slot.info.generation + 1;
        slot.info = {}; slot.info.generation = generation; slot.info.flags = present ? 1 : 0;
        slot.last_descriptor_ms = now - 2000;
    }
    if (stabilizing && (slot.info.flags & 16)) slot.last_descriptor_ms = now - 2000;
    const bool read = slot.debounce == 3 && present && now - slot.last_descriptor_ms >= 2000;
    if (read) slot.last_descriptor_ms = now;
    Unlock();
    if (read) {
        uint8_t raw[0x86]; bool success = true;
        // Release the I2C bus between short chunks so v1.0 touch can interleave.
        for (size_t offset = 0; success && offset < sizeof(raw); offset += 16) {
            const uint8_t address = offset;
            success = i2c_master_transmit_receive(slot.eeprom, &address, 1, raw+offset,
                std::min<size_t>(16, sizeof(raw)-offset), 5) == ESP_OK;
        }
        pxa_peripheral_module_t info = {};
        const bool valid = success && mosaico_board::DecodeModule(raw, sizeof(raw), &info);
        xSemaphoreTake(g_mutex, portMAX_DELAY);
        info.flags = valid ? 3 : (1 | 16);
        info.generation = slot.info.generation;
        if (std::memcmp(&slot.info, &info, sizeof(info)) != 0) ++info.generation;
        slot.info = info;
        Unlock();
    }
    xSemaphoreTake(g_mutex, portMAX_DELAY); slot.scanning = false; Unlock();
}
TickType_t NextWait(uint32_t now, uint32_t last_scan) {
    uint32_t delay_ms = UINT32_MAX;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    for (const auto& device : g_devices) {
        if (!device.live) continue;
        if (device.opening || device.pending) delay_ms = std::min<uint32_t>(delay_ms, 1);
        if (device.closing.load() || (device.prepared && device.open_result != PXA_STATUS_OK && device.touched))
            delay_ms = std::min<uint32_t>(delay_ms, 20);
    }
    bool scan = false;
    for (const auto& slot : g_slots) {
        if (slot.restoring) delay_ms = std::min<uint32_t>(delay_ms, 20);
        else if (g_bus && !slot.references) scan = true;
    }
    if (scan && !g_shutdown.load()) {
        const uint32_t elapsed = now - last_scan;
        delay_ms = std::min<uint32_t>(delay_ms, elapsed >= 250 ? 1 : 250 - elapsed);
    }
    Unlock();
    return delay_ms == UINT32_MAX ? portMAX_DELAY : std::max<TickType_t>(1, pdMS_TO_TICKS(delay_ms));
}
void Worker(void*) {
    uint32_t last_scan = 0;
    for (;;) {
        for (auto& device : g_devices) {
            if (g_shutdown.load()) {
                xSemaphoreTake(g_mutex, portMAX_DELAY);
                device.closing = device.live;
                Unlock();
            }
            Prepare(device); Transfer(device);
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            if (device.cancelled.load() && device.complete) device.complete = false;
            Unlock();
            Cleanup(device);
        }
        for (unsigned i = 0; i < 2; ++i) {
            if (g_slots[i].restoring && ConfigureSelector(i)) {
                xSemaphoreTake(g_mutex, portMAX_DELAY);
                g_slots[i].restoring = false; g_slots[i].info.flags = 8;
                g_slots[i].candidate = -1; g_slots[i].debounce = 0;
                ++g_slots[i].info.generation; Unlock();
            }
        }
        const uint32_t now = esp_timer_get_time() / 1000;
        if (!g_shutdown.load() && now - last_scan >= 250) { Scan(0, now); Scan(1, now); last_scan = now; }
        if (g_shutdown.load()) {
            bool live = false;
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            for (const auto& device : g_devices) live |= device.live;
            Unlock();
            if (!live) break;
        }
        (void)ulTaskNotifyTake(pdTRUE, NextWait(uint32_t(esp_timer_get_time() / 1000), last_scan));
    }
    g_notifications.Stop();
    g_stopped = true;
    vTaskDelete(nullptr);
}
}

bool EspMosaicoInitializePeripherals(i2c_master_bus_handle_t onboard, uint16_t revision, bool usb_ready) {
    if (g_initialized) return !g_shutdown.load();
    g_port_count = 0;
    g_revision = revision; g_usb_ready = usb_ready;
    g_mutex = xSemaphoreCreateMutexStatic(&g_mutex_storage);
    if (!g_mutex || pxa_esp_resource_memory_initialize() != PXA_STATUS_OK) return false;
    auto* buffers = static_cast<uint8_t*>(pxa_memory_allocate(
        pxa_esp_device_resource_allocator(PXA_MEMORY_EXTERNAL, PXA_MEMORY_METADATA), kDevices*512));
    if (!buffers) return false;
    for (size_t i = 0; i < kDevices; ++i) { g_devices[i].tx = buffers + i*512; g_devices[i].rx = buffers + i*512+256; }
    if (revision == 0x0100) g_bus = onboard;
    else {
        i2c_master_bus_config_t config = {};
        config.i2c_port = I2C_NUM_1; config.sda_io_num = GPIO_NUM_0; config.scl_io_num = GPIO_NUM_1;
        config.clk_source = I2C_CLK_SRC_DEFAULT; config.glitch_ignore_cnt = 7;
        config.flags.enable_internal_pullup = true;
        if (i2c_new_master_bus(&config, &g_bus) != ESP_OK) g_bus = nullptr;
    }
    AddPort(1, PXA_PERIPHERALS_SLOT, 1, "left.slot"); AddPort(2, PXA_PERIPHERALS_SLOT, 1, "right.slot");
    AddPort(3, PXA_PERIPHERALS_I2C, g_bus ? 3 : 4, "expansion.i2c");
    AddPort(4, PXA_PERIPHERALS_UART, usb_ready ? 3 : 4, "right.uart");
    for (size_t i = 0; i < std::size(kLeftPins); ++i) {
        char name[32]; std::snprintf(name, sizeof(name), "left.gpio%u", unsigned(i+1)); AddPort(16+i, 1, 3, name);
    }
    for (size_t i = 0; i < std::size(kRightPins); ++i) {
        char name[32]; std::snprintf(name, sizeof(name), "right.gpio%u", unsigned(i+1)); AddPort(32+i, 1, 3, name);
    }
    if (revision == 0x0100) {
        gpio_config_t config = {};
        config.pin_bit_mask = 1ULL << 3; config.mode = GPIO_MODE_OUTPUT;
        if (gpio_set_level(GPIO_NUM_3, 1) == ESP_OK && gpio_config(&config) == ESP_OK)
            AddPort(48, 1, 3, "board.status-led");
    }
    for (unsigned i = 0; i < 2; ++i) {
        g_slots[i].info.flags = g_bus ? 8 : (8 | 16); g_slots[i].info.generation = 1;
        if (!ConfigureSelector(i)) g_slots[i].restoring = true;
        if (g_bus) {
            i2c_device_config_t config = {};
            config.dev_addr_length = I2C_ADDR_BIT_LEN_7; config.device_address = 0x50+i; config.scl_speed_hz = 100000;
            g_slots[i].device_attempted = true;
            g_slots[i].last_device_attempt_ms = uint32_t(esp_timer_get_time() / 1000);
            if (i2c_master_bus_add_device(g_bus, &config, &g_slots[i].eeprom) != ESP_OK) g_slots[i].info.flags |= 16;
        }
    }
    g_task = xTaskCreateStatic(Worker, "mosaico_ports", kStackBytes, nullptr, 2, g_stack, &g_task_storage);
    if (!g_task) {
        // No worker can own these devices yet. Preserve allocations on a native
        // cleanup failure rather than leaving a driver with freed state.
        bool cleaned = true;
        for (auto& slot : g_slots) if (slot.eeprom) {
            if (i2c_master_bus_rm_device(slot.eeprom) == ESP_OK) slot.eeprom = nullptr;
            else cleaned = false;
        }
        if (cleaned && revision != 0x0100 && g_bus) cleaned = i2c_del_master_bus(g_bus) == ESP_OK;
        if (cleaned) {
            g_bus = nullptr; pxa_memory_release(buffers);
            for (auto& device : g_devices) device.tx = device.rx = nullptr;
        } else { g_initialized = true; g_shutdown = true; }
        return false;
    }
    g_initialized = true;
    pxa_peripherals_backend_t backend = {};
    backend.struct_size = sizeof(backend); backend.list = List; backend.module = Module; backend.open = Open;
    backend.io = Io; backend.start = Start; backend.poll = Poll; backend.cancel = Cancel; backend.close = Close; backend.ready = Ready;
    return pxa_esp_peripherals_set_backend(&backend);
}

bool EspMosaicoStopPeripherals() {
    if (!g_task) return true;
    if (!g_shutdown.exchange(true)) (void)g_notifications.Notify(g_task);
    const int64_t deadline = esp_timer_get_time() + 2000000;
    while (!g_stopped.load()) {
        if (esp_timer_get_time() >= deadline) return false;
        vTaskDelay(1);
    }
    return true;
}
