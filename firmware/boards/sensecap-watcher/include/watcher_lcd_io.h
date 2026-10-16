#pragma once

#include <esp_lcd_io_spi.h>

// SPD2010's write-only QSPI transport, using the public SPI driver. Packs
// native game pixels during bounded DMA submissions instead of mirroring a
// full PSRAM frame first. Only the board's flush owner calls this interface.
esp_err_t WatcherLcdIoCreate(spi_host_device_t bus,
    const esp_lcd_panel_io_spi_config_t& config, esp_lcd_panel_io_handle_t* output);

// Two DMA-capable blocks are borrowed, not allocated; a block is recycled
// only after its SPI transaction completes. A null pack copies wire pixels,
// while a pack callback can convert native pixels directly into these blocks.
using WatcherLcdPack = void (*)(void* context, const void* source,
    void* destination, size_t offset_bytes, size_t size_bytes);
void WatcherLcdIoSetPacking(esp_lcd_panel_io_handle_t io, WatcherLcdPack pack,
    void* context, void* scratch, size_t block_bytes);
