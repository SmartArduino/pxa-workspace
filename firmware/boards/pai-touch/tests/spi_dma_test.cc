// SPDX-License-Identifier: Apache-2.0
#include "pai_touch_display_dma.h"
#include <cassert>
#include <cstdio>
#include <cstring>
static int panel_storage, other_storage;
static esp_err_t returned_status = ESP_OK;
static spi_transaction_t* returned = nullptr;
static esp_err_t queued_status = ESP_OK;
extern "C" esp_err_t __real_spi_bus_get_max_transaction_len(spi_host_device_t, size_t* n) { *n = 4092; return ESP_OK; }
extern "C" esp_err_t __wrap_spi_bus_get_max_transaction_len(spi_host_device_t, size_t*);
extern "C" esp_err_t __real_spi_bus_add_device(spi_host_device_t host, const spi_device_interface_config_t*, spi_device_handle_t* out) {
    *out = host == 2 ? &panel_storage : &other_storage; return ESP_OK;
}
extern "C" esp_err_t __real_spi_device_get_trans_result(spi_device_handle_t, spi_transaction_t** out, TickType_t) {
    if (returned) *out = returned;
    return returned_status;
}
extern "C" esp_err_t __real_spi_device_queue_trans(spi_device_handle_t, spi_transaction_t*, TickType_t) { return queued_status; }
extern "C" esp_err_t __wrap_spi_bus_add_device(spi_host_device_t, const spi_device_interface_config_t*, spi_device_handle_t*);
extern "C" esp_err_t __wrap_spi_device_get_trans_result(spi_device_handle_t, spi_transaction_t**, TickType_t);
extern "C" esp_err_t __wrap_spi_device_queue_trans(spi_device_handle_t, spi_transaction_t*, TickType_t);
int main() {
    spi_device_handle_t panel, other;
    size_t maximum = 0;
    PaiTouchDisplayDmaBeginBinding(2);
    assert(__wrap_spi_bus_get_max_transaction_len(2, &maximum) == ESP_OK && maximum == 1920);
    assert(__wrap_spi_bus_add_device(2, nullptr, &panel) == ESP_OK);
    PaiTouchDisplayDmaEndBinding();
    assert(__wrap_spi_bus_get_max_transaction_len(2, &maximum) == ESP_OK && maximum == 4092);
    assert(__wrap_spi_bus_add_device(3, nullptr, &other) == ESP_OK);
    uint8_t pixels[1920]{}; pixels[12] = 42;
    spi_transaction_t transaction{SPI_TRANS_DMA_USE_PSRAM | SPI_TRANS_DMA_TX_FAIL, 15360, pixels};
    returned = &transaction; returned_status = ESP_ERR_INVALID_STATE;
    spi_transaction_t* output = nullptr;
    unsigned inflight = 1;
    auto result = __wrap_spi_device_get_trans_result(panel, &output, 1);
    if (result == ESP_OK) --inflight;
    assert(!inflight && output == returned && PaiTouchDisplayDmaFaults() == 1);
    assert(!PaiTouchDisplayDmaUsesStaging());
    assert(__wrap_spi_device_get_trans_result(other, &output, 1) == ESP_ERR_INVALID_STATE);
    assert(PaiTouchDisplayDmaFaults() == 1);
    assert(__wrap_spi_device_get_trans_result(panel, &output, 1) == ESP_OK);
    assert(PaiTouchDisplayDmaUsesStaging() && PaiTouchDisplayDmaFaults() == 2);
    transaction.flags = SPI_TRANS_DMA_USE_PSRAM | 7;
    assert(__wrap_spi_device_queue_trans(other, &transaction, 1) == ESP_OK);
    assert(transaction.flags & SPI_TRANS_DMA_USE_PSRAM);
    assert(__wrap_spi_device_queue_trans(panel, &transaction, 1) == ESP_OK);
    assert(transaction.flags == 7);
    assert(transaction.tx_buffer != pixels && !std::memcmp(transaction.tx_buffer, pixels, sizeof(pixels)));
    returned_status = ESP_OK;
    assert(__wrap_spi_device_get_trans_result(panel, &output, 1) == ESP_OK);
    assert(transaction.tx_buffer == pixels);
    // Two distinct in-flight descriptors retain their own SRAM pixels.
    spi_transaction_t second{SPI_TRANS_DMA_USE_PSRAM, 15360, pixels};
    spi_transaction_t third{SPI_TRANS_DMA_USE_PSRAM, 15360, pixels};
    transaction.flags = SPI_TRANS_DMA_USE_PSRAM;
    assert(__wrap_spi_device_queue_trans(panel, &transaction, 1) == ESP_OK);
    assert(__wrap_spi_device_queue_trans(panel, &second, 1) == ESP_OK);
    assert(transaction.tx_buffer != second.tx_buffer);
    assert(__wrap_spi_device_queue_trans(panel, &third, 1) == ESP_ERR_NO_MEM);
    assert(third.tx_buffer == pixels);
    returned = &transaction;
    assert(__wrap_spi_device_get_trans_result(panel, &output, 1) == ESP_OK && transaction.tx_buffer == pixels);
    returned = &second;
    assert(__wrap_spi_device_get_trans_result(panel, &output, 1) == ESP_OK && second.tx_buffer == pixels);
    queued_status = ESP_ERR_NO_MEM;
    assert(__wrap_spi_device_queue_trans(panel, &third, 1) == ESP_ERR_NO_MEM && third.tx_buffer == pixels);
    queued_status = ESP_OK; third.flags = SPI_TRANS_DMA_USE_PSRAM; third.length = 32768;
    assert(__wrap_spi_device_queue_trans(panel, &third, 1) == ESP_ERR_INVALID_ARG);
    returned = nullptr; returned_status = ESP_ERR_TIMEOUT;
    output = &transaction;
    assert(__wrap_spi_device_get_trans_result(panel, &output, 1) == ESP_ERR_TIMEOUT);
    assert(!output && PaiTouchDisplayDmaFaults() == 2);
    returned_status = ESP_ERR_NO_MEM;
    assert(__wrap_spi_device_get_trans_result(panel, &output, 1) == ESP_ERR_NO_MEM);
    assert(PaiTouchDisplayDmaFaults() == 2);
    returned = &transaction; returned_status = ESP_ERR_INVALID_STATE; transaction.flags = 0;
    assert(__wrap_spi_device_get_trans_result(panel, &output, 1) == ESP_ERR_INVALID_STATE);
    assert(PaiTouchDisplayDmaFaults() == 2);
    puts("PAI LCD DMA: completed-error dequeue, exact in-flight ownership, device isolation, staged fallback and non-completed failures passed");
}
