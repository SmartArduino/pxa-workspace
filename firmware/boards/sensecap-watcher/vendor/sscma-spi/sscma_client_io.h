// SPDX-License-Identifier: Apache-2.0
// SPI-only declaration, derived from Seeed's sscma_client_io.h.
#pragma once
#include <stddef.h>
#include <stdbool.h>
#include "sscma_client_io_interface.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef void* sscma_client_spi_bus_handle_t;
typedef sscma_client_io_t* sscma_client_io_handle_t;
typedef struct {
    int cs_gpio_num, sync_gpio_num, spi_mode, wait_delay;
    unsigned pclk_hz;
    size_t trans_queue_depth;
    void* user_ctx;
    struct { unsigned octal_mode:1, sio_mode:1, lsb_first:1, cs_high_active:1; } flags;
} sscma_client_io_spi_config_t;
esp_err_t sscma_client_new_io_spi_bus(sscma_client_spi_bus_handle_t,
    const sscma_client_io_spi_config_t*, sscma_client_io_handle_t*);
#ifdef __cplusplus
}
#endif
