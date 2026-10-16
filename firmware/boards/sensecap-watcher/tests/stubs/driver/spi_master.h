#pragma once
#include <cstddef>
#include <cstdint>
#include <esp_err.h>
using spi_host_device_t = int;
using spi_device_handle_t = void*;
inline constexpr int SPI_DEVICE_HALFDUPLEX=1, SPI_DEVICE_POSITIVE_CS=2;
inline constexpr int SPI_TRANS_USE_TXDATA=1, SPI_TRANS_CS_KEEP_ACTIVE=2,
    SPI_TRANS_MODE_QIO=4, SPI_TRANS_DMA_USE_PSRAM=8;
struct spi_transaction_t {
    unsigned flags{}; size_t length{}; const void* tx_buffer{};
    uint8_t tx_data[4]{}; void* user{};
};
struct spi_device_interface_config_t {
    int clock_speed_hz{}, mode{}, spics_io_num{}, queue_size{}, flags{};
    int cs_ena_pretrans{}, cs_ena_posttrans{};
    void (*post_cb)(spi_transaction_t*){};
};
esp_err_t spi_device_get_trans_result(spi_device_handle_t, spi_transaction_t**, uint32_t);
esp_err_t spi_device_polling_transmit(spi_device_handle_t, spi_transaction_t*);
esp_err_t spi_device_acquire_bus(spi_device_handle_t, uint32_t);
void spi_device_release_bus(spi_device_handle_t);
esp_err_t spi_device_queue_trans(spi_device_handle_t, spi_transaction_t*, uint32_t);
esp_err_t spi_bus_remove_device(spi_device_handle_t);
esp_err_t spi_bus_get_max_transaction_len(spi_host_device_t, size_t*);
esp_err_t spi_bus_add_device(spi_host_device_t, const spi_device_interface_config_t*, spi_device_handle_t*);
