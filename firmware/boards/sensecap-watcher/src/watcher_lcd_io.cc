#include "watcher_lcd_io.h"

#include <algorithm>
#include <new>
#include <esp_attr.h>
#include <esp_lcd_panel_io_interface.h>
#include <freertos/FreeRTOS.h>

namespace {
constexpr size_t kSlots = 2;

// First-member base implements the public esp_lcd transport interface. Only
// the flush owner submits pixels; polling panel commands drain queued work.
struct LcdIo {
    esp_lcd_panel_io_t base{};
    spi_device_handle_t device{};
    size_t max_bytes{};
    size_t inflight{};
    struct Slot {
        spi_transaction_t transaction{};
        LcdIo* owner{};
        bool last{};
    } slots[kSlots];
    esp_lcd_panel_io_color_trans_done_cb_t completed{};
    void* completed_context{};
    WatcherLcdPack pack{};
    void* pack_context{};
    uint8_t* scratch{};
    size_t block_bytes{};

    static LcdIo& Get(esp_lcd_panel_io_t* io) {
        return *reinterpret_cast<LcdIo*>(io);
    }

    esp_err_t Drain() {
        esp_err_t first_error = ESP_OK;
        while (inflight != 0) {
            spi_transaction_t* result{};
            const auto error = spi_device_get_trans_result(device, &result, portMAX_DELAY);
            // DMA errors still return a completed descriptor. Recycle it and
            // drain the others; returning early would leave a phantom inflight
            // count and deadlock the next command after an underrun.
            if (!result) return error;
            --inflight;
            if (first_error == ESP_OK) first_error = error;
        }
        return first_error;
    }

    esp_err_t Command(int command, bool keep_cs) {
        if (command < 0) return ESP_OK;
        spi_transaction_t transaction{};
        transaction.length = 32;
        transaction.flags = SPI_TRANS_USE_TXDATA |
            (keep_cs ? SPI_TRANS_CS_KEEP_ACTIVE : 0);
        const auto value = __builtin_bswap32(static_cast<uint32_t>(command));
        __builtin_memcpy(transaction.tx_data, &value, sizeof(value));
        return spi_device_polling_transmit(device, &transaction);
    }

    static void IRAM_ATTR Complete(spi_transaction_t* transaction) {
        if (transaction->user == nullptr) return;
        auto& slot = *static_cast<Slot*>(transaction->user);
        auto& self = *slot.owner;
        if (slot.last && self.completed &&
            self.completed(&self.base, nullptr, self.completed_context))
            portYIELD_FROM_ISR();
    }

    static esp_err_t Parameters(esp_lcd_panel_io_t* io, int command,
                               const void* bytes, size_t size) {
        auto& self = Get(io);
        if (size != 0 && bytes == nullptr) return ESP_ERR_INVALID_ARG;
        auto error = spi_device_acquire_bus(self.device, portMAX_DELAY);
        if (error != ESP_OK) return error;
        error = self.Drain();
        if (error == ESP_OK) error = self.Command(command, size != 0);
        size_t offset = 0;
        while (error == ESP_OK && offset < size) {
            spi_transaction_t transaction{};
            const auto count = std::min(size - offset, sizeof(transaction.tx_data));
            transaction.length = count * 8;
            transaction.flags = SPI_TRANS_USE_TXDATA |
                (offset + count < size ? SPI_TRANS_CS_KEEP_ACTIVE : 0);
            // Small panel parameters need no DMA allocation. Copy only valid
            // bytes, including an odd tail, instead of driver's padded read.
            __builtin_memcpy(transaction.tx_data,
                static_cast<const uint8_t*>(bytes) + offset, count);
            error = spi_device_polling_transmit(self.device, &transaction);
            offset += count;
        }
        spi_device_release_bus(self.device);
        return error;
    }

    static esp_err_t Color(esp_lcd_panel_io_t* io, int command,
                          const void* pixels, size_t bytes) {
        auto& self = Get(io);
        if (!pixels || bytes == 0) return ESP_ERR_INVALID_ARG;
        auto error = spi_device_acquire_bus(self.device, portMAX_DELAY);
        if (error != ESP_OK) return error;
        error = self.Drain();
        if (error == ESP_OK) error = self.Command(command, true);
        // Partial UI redraws share SRAM with font/image decoding and capture.
        // Bound their temporary DMA allocations even if the bus allows more.
        const size_t chunk = self.scratch ? self.block_bytes : std::min(self.max_bytes, size_t{4096});
        size_t offset = 0;
        while (error == ESP_OK && offset < bytes) {
            Slot* slot{};
            if (self.inflight < kSlots) {
                slot = &self.slots[self.inflight];
            } else {
                spi_transaction_t* result{};
                error = spi_device_get_trans_result(self.device, &result, portMAX_DELAY);
                if (result) --self.inflight;
                if (error != ESP_OK) break;
                slot = static_cast<Slot*>(result->user);
            }
            const size_t size = std::min(chunk, bytes - offset);
            const void* source = static_cast<const uint8_t*>(pixels) + offset;
            if (self.scratch) {
                auto* destination = self.scratch + (slot - self.slots) * chunk;
                if (self.pack)
                    self.pack(self.pack_context, pixels, destination, offset, size);
                else
                    __builtin_memcpy(destination, source, size);
                source = destination;
            }
            slot->last = offset + size == bytes;
            slot->transaction = {};
            auto& transaction = slot->transaction;
            transaction.user = slot;
            transaction.tx_buffer = source;
            transaction.length = size * 8;
            transaction.flags = SPI_TRANS_MODE_QIO |
                (slot->last ? 0 : SPI_TRANS_CS_KEEP_ACTIVE);
            // The board lends DMA-capable SRAM, so the driver neither
            // allocates nor copies pixels. Direct PSRAM DMA underruns on this
            // board's shared memory bus; no external-DMA flag is used.
            error = spi_device_queue_trans(self.device, &transaction, portMAX_DELAY);
            if (error == ESP_OK) {
                ++self.inflight;
                offset += size;
            }
        }
        // A failed enqueue can leave earlier chunks reading the scratch or
        // source. Settle them before the caller releases/reuses that storage.
        const auto drained = self.Drain();
        if (error == ESP_OK) error = drained;
        spi_device_release_bus(self.device);
        return error;
    }

    static esp_err_t Delete(esp_lcd_panel_io_t* io) {
        auto& self = Get(io);
        const auto error = self.Drain();
        if (error != ESP_OK) return error;
        const auto removed = spi_bus_remove_device(self.device);
        if (removed == ESP_OK) delete &self;
        return removed;
    }
};
} // namespace

esp_err_t WatcherLcdIoCreate(spi_host_device_t bus,
    const esp_lcd_panel_io_spi_config_t& config, esp_lcd_panel_io_handle_t* output) {
    if (!output || config.dc_gpio_num >= 0 || !config.flags.quad_mode ||
        config.flags.octal_mode || config.flags.sio_mode || config.flags.lsb_first ||
        config.lcd_cmd_bits != 32 || config.lcd_param_bits != 8 ||
        config.trans_queue_depth != kSlots) return ESP_ERR_INVALID_ARG;
    *output = nullptr;
    auto* self = new(std::nothrow) LcdIo{};
    if (!self) return ESP_ERR_NO_MEM;
    spi_device_interface_config_t device{};
    device.clock_speed_hz = config.pclk_hz;
    device.mode = config.spi_mode;
    device.spics_io_num = config.cs_gpio_num;
    device.queue_size = kSlots;
    device.flags = SPI_DEVICE_HALFDUPLEX |
        (config.flags.cs_high_active ? SPI_DEVICE_POSITIVE_CS : 0);
    device.cs_ena_pretrans = config.cs_ena_pretrans;
    device.cs_ena_posttrans = config.cs_ena_posttrans;
    device.post_cb = LcdIo::Complete;
    auto error = spi_bus_add_device(bus, &device, &self->device);
    if (error == ESP_OK) error = spi_bus_get_max_transaction_len(bus, &self->max_bytes);
    if (error != ESP_OK) {
        if (self->device) (void)spi_bus_remove_device(self->device);
        delete self;
        return error;
    }
    for (auto& slot : self->slots) slot.owner = self;
    self->completed = config.on_color_trans_done;
    self->completed_context = config.user_ctx;
    self->base.rx_param = [](esp_lcd_panel_io_t*, int, void*, size_t) {
        return ESP_ERR_NOT_SUPPORTED;
    };
    self->base.tx_param = LcdIo::Parameters;
    self->base.tx_color = LcdIo::Color;
    self->base.del = LcdIo::Delete;
    self->base.register_event_callbacks = [](esp_lcd_panel_io_t* io,
        const esp_lcd_panel_io_callbacks_t* callbacks, void* context) -> esp_err_t {
        if (!callbacks) return ESP_ERR_INVALID_ARG;
        auto& self = LcdIo::Get(io);
        self.completed = callbacks->on_color_trans_done;
        self.completed_context = context;
        return ESP_OK;
    };
    *output = &self->base;
    return ESP_OK;
}

void WatcherLcdIoSetPacking(esp_lcd_panel_io_handle_t io, WatcherLcdPack pack,
    void* context, void* scratch, size_t block_bytes) {
    auto& self = LcdIo::Get(io);
    self.pack = pack;
    self.pack_context = context;
    self.scratch = static_cast<uint8_t*>(scratch);
    self.block_bytes = block_bytes;
}
