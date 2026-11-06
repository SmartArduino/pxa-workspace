// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>
typedef int esp_err_t;
typedef int spi_host_device_t;
typedef void* spi_device_handle_t;
typedef uint32_t TickType_t;
typedef struct { int unused; } spi_device_interface_config_t;
typedef struct { uint32_t flags; uint32_t length; const void* tx_buffer; } spi_transaction_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_TIMEOUT 0x107
#define SPI_TRANS_DMA_USE_PSRAM (1u << 12)
#define SPI_TRANS_DMA_RX_FAIL (1u << 30)
#define SPI_TRANS_DMA_TX_FAIL (1u << 31)
