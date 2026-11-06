// SPDX-License-Identifier: Apache-2.0
#include "pai_touch_display_dma.h"
#include <atomic>
#include <array>
#include <cstring>

namespace {
std::atomic<int> binding_host{-1};
std::atomic<spi_device_handle_t> panel_device{nullptr};
std::atomic<uint32_t> faults{0};
std::atomic<bool> staging{false};
// Reserved before camera/Wi-Fi allocations fragment internal SRAM. Two slots
// match the LCD IO queue depth. Each holds four physical 240-pixel lines.
struct Stage {
    alignas(16) std::array<uint8_t, 1920> bytes;
    spi_transaction_t* transaction = nullptr;
    const void* source = nullptr;
};
Stage stages[2];
void release_stage(spi_transaction_t* transaction) {
    for (auto& slot : stages) if (slot.transaction == transaction) {
        transaction->tx_buffer = slot.source;
        slot.transaction = nullptr;
        return;
    }
}
#if PXA_PAI_DISPLAY_DMA_TEST_FAULTS > 0
std::atomic<bool> camera_test_armed{false};
#endif
}

void PaiTouchDisplayDmaBeginBinding(spi_host_device_t host) { binding_host.store(int(host)); }
void PaiTouchDisplayDmaEndBinding() { binding_host.store(-1); }
uint32_t PaiTouchDisplayDmaFaults() { return faults.load(); }
bool PaiTouchDisplayDmaUsesStaging() { return staging.load(); }
#if PXA_PAI_DISPLAY_DMA_TEST_FAULTS > 0
void PaiTouchDisplayDmaArmCameraTest() { camera_test_armed.store(true); }
#endif

extern "C" {
esp_err_t __real_spi_bus_get_max_transaction_len(spi_host_device_t, size_t*);
esp_err_t __real_spi_bus_add_device(spi_host_device_t, const spi_device_interface_config_t*, spi_device_handle_t*);
esp_err_t __real_spi_device_get_trans_result(spi_device_handle_t, spi_transaction_t**, uint32_t);
esp_err_t __real_spi_device_queue_trans(spi_device_handle_t, spi_transaction_t*, uint32_t);

esp_err_t __wrap_spi_bus_get_max_transaction_len(spi_host_device_t host, size_t* bytes) {
    const auto result = __real_spi_bus_get_max_transaction_len(host, bytes);
    // IDF rounds a requested 1920-byte bus limit to a full DMA descriptor
    // (4092 bytes). Keep the LCD chunk size within our reserved fallback slots.
    if (result == ESP_OK && bytes && binding_host.load() == int(host) && *bytes > stages[0].bytes.size())
        *bytes = stages[0].bytes.size();
    return result;
}

esp_err_t __wrap_spi_bus_add_device(spi_host_device_t host, const spi_device_interface_config_t* config,
                                  spi_device_handle_t* output) {
    const auto result = __real_spi_bus_add_device(host, config, output);
    if (result == ESP_OK && output && binding_host.load() == int(host)) panel_device.store(*output);
    return result;
}

esp_err_t __wrap_spi_device_get_trans_result(spi_device_handle_t device, spi_transaction_t** output,
                                            uint32_t timeout) {
    if (!device || device != panel_device.load())
        return __real_spi_device_get_trans_result(device, output, timeout);
    if (!output) return ESP_ERR_INVALID_ARG;
    *output = nullptr;
    auto result = __real_spi_device_get_trans_result(device, output, timeout);
#if PXA_PAI_DISPLAY_DMA_TEST_FAULTS > 0
    // Test firmware only. The LCD submitter is the sole transaction owner.
    // Inject the same error after a real completed/dequeued color transfer.
    static unsigned remaining = PXA_PAI_DISPLAY_DMA_TEST_FAULTS;
    if (remaining && camera_test_armed.load() && result == ESP_OK && *output &&
        ((*output)->flags & SPI_TRANS_DMA_USE_PSRAM) && (*output)->length > 256) {
        --remaining;
        (*output)->flags |= SPI_TRANS_DMA_TX_FAIL;
        result = ESP_ERR_INVALID_STATE;
    }
#endif
    if (*output) release_stage(*output);
    if (result == ESP_ERR_INVALID_STATE && *output &&
        ((*output)->flags & (SPI_TRANS_DMA_TX_FAIL | SPI_TRANS_DMA_RX_FAIL))) {
        // IDF already dequeued this descriptor and freed its bounce buffers.
        // esp_lcd decrements num_trans_inflight only after ESP_OK. Report the
        // dequeue here so it cannot wait forever for the same transaction.
        // The presenter observes the separate fault epoch, drops this frame
        // from completion accounting, and retries only after all DMA drains.
        if (faults.fetch_add(1) + 1 >= 2) staging.store(true);
        return ESP_OK;
    }
    return result;
}

esp_err_t __wrap_spi_device_queue_trans(spi_device_handle_t device, spi_transaction_t* transaction,
                                       uint32_t timeout) {
    if (!device || device != panel_device.load() || !transaction || !staging.load() ||
        !(transaction->flags & SPI_TRANS_DMA_USE_PSRAM))
        return __real_spi_device_queue_trans(device, transaction, timeout);
    const auto size = (transaction->length + 7) / 8;
    if (!transaction->tx_buffer || size > stages[0].bytes.size()) return ESP_ERR_INVALID_ARG;
    for (auto& slot : stages) if (!slot.transaction) {
        slot.source = transaction->tx_buffer;
        slot.transaction = transaction;
        std::memcpy(slot.bytes.data(), slot.source, size);
        transaction->tx_buffer = slot.bytes.data();
        transaction->flags &= ~SPI_TRANS_DMA_USE_PSRAM;
        const auto result = __real_spi_device_queue_trans(device, transaction, timeout);
        if (result != ESP_OK) release_stage(transaction);
        return result;
    }
    return ESP_ERR_NO_MEM;
}
}
