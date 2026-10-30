// SPDX-License-Identifier: MIT
#include "watcher_hardware.h"
#include "sensecap_watcher_config.h"
#include <driver/sdspi_host.h>
#include <driver/spi_master.h>
#include <esp_log.h>
#include <pxa_board_common/sd_volume.h>
#include <sdkconfig.h>

// Pinout and active-low detection verified against Seeed's Watcher BSP,
// 8e37f7ccd3d836bb99f483e0f6e74e8252406644. SPI2 is shared with Himax;
// this board currently owns its initialization (LCD is on SPI3).
bool SensecapWatcherHardware::InitializeSharedSpi() {
    static bool initialized = false;
    if (initialized) return true;
    // Both devices are powered. Deassert their selects before initializing the
    // shared controller, including when no SD card is inserted.
    gpio_config_t ai_select = {};
    ai_select.pin_bit_mask = (UINT64_C(1) << WATCHER_AI_SPI_CS) | (UINT64_C(1) << WATCHER_SD_CS);
    ai_select.mode = GPIO_MODE_OUTPUT;
    ai_select.pull_up_en = GPIO_PULLUP_ENABLE;
    if (gpio_set_level(WATCHER_AI_SPI_CS, 1) != ESP_OK ||
        gpio_set_level(WATCHER_SD_CS, 1) != ESP_OK || gpio_config(&ai_select) != ESP_OK)
        return false;
    spi_bus_config_t bus = {};
    bus.sclk_io_num = WATCHER_SD_CLK;
    bus.mosi_io_num = WATCHER_SD_MOSI;
    bus.miso_io_num = WATCHER_SD_MISO;
    bus.quadwp_io_num = bus.quadhd_io_num = -1;
    bus.data4_io_num = bus.data5_io_num = bus.data6_io_num = bus.data7_io_num = -1;
    bus.max_transfer_sz = 4096;
    const auto error = spi_bus_initialize(WATCHER_SD_HOST, &bus, SPI_DMA_CH_AUTO);
    if (error != ESP_OK) {
        ESP_LOGW("watcher_spi", "SPI2 initialization failed: %s", esp_err_to_name(error));
        return false;
    }
    initialized = true;
    return true;
}
bool SensecapWatcherHardware::InitializeSd() {
    constexpr char tag[] = "watcher_sd";
    constexpr char mount[] = CONFIG_PXA_MOUNT_POINT "/sdcard";
    static sdmmc_card_t* card = nullptr;
    if (card) return true;
    if (!InitializeSharedSpi()) return false;
    bool absent = true;
    if (!io_expander_.InputLevel(WATCHER_IO_SD_DET, &absent)) return false;
    if (absent) {
        ESP_LOGI(tag, "No SD card; slot is enabled, insert a FAT card and restart to mount");
        return true;
    }
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = WATCHER_SD_HOST;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = WATCHER_SD_HOST;
    slot.gpio_cs = WATCHER_SD_CS;
    const esp_vfs_fat_sdmmc_mount_config_t config = {
        .format_if_mount_failed = false, .max_files = 8, .allocation_unit_size = 16 * 1024,
    };
    const auto mounted = esp_vfs_fat_sdspi_mount(mount, &host, &slot, &config, &card);
    if (mounted != ESP_OK) {
        card = nullptr;
        ESP_LOGW(tag, "SD unavailable: %s; card was not formatted", esp_err_to_name(mounted));
        return false;
    }
    const auto status = pxa_board::RegisterSdVolume(mount, card);
    if (status != PXA_STATUS_OK) {
        ESP_LOGW(tag, "SD volume registration failed: %ld", long(status));
        if (esp_vfs_fat_sdcard_unmount(mount, card) == ESP_OK) {
            card = nullptr;
        }
        return false;
    }
    ESP_LOGI(tag, "SD ready at %s, SPI2 20 MHz", mount);
    return true;
}
