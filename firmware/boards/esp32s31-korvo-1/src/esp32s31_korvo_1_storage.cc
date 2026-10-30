// SPDX-License-Identifier: MIT
#include "esp32s31_korvo_1_config.h"
#include <driver/sdmmc_host.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <pxa_board_common/sd_volume.h>
#include <sdkconfig.h>

// Pinout/polarity: Espressif esp-bsp 67f41a169401d1f23e827188dd08fb351cb0febe,
// esp32_s31_korvo_1: bsp_storage.c, bsp_feature_en.c and the board header.
void Esp32S31Korvo1InitializeSd() {
    constexpr char tag[] = "korvo_sd";
    constexpr char mount[] = CONFIG_PXA_MOUNT_POINT "/sdcard";
    static sdmmc_card_t* card = nullptr;
    if (card) return;
    gpio_config_t power = {};
    power.pin_bit_mask = UINT64_C(1) << KORVO_SD_ENABLE;
    power.mode = GPIO_MODE_OUTPUT;
    // SD_EN is active low. Set its latch before switching to output.
    if (gpio_set_level(KORVO_SD_ENABLE, 0) != ESP_OK || gpio_config(&power) != ESP_OK) {
        ESP_LOGW(tag, "SD power control unavailable");
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.flags = SDMMC_HOST_FLAG_4BIT | SDMMC_HOST_FLAG_DEINIT_ARG;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk = KORVO_SD_CLK; slot.cmd = KORVO_SD_CMD;
    slot.d0 = KORVO_SD_D0; slot.d1 = KORVO_SD_D1;
    slot.d2 = KORVO_SD_D2; slot.d3 = KORVO_SD_D3;
    slot.width = 4;
    const esp_vfs_fat_sdmmc_mount_config_t config = {
        .format_if_mount_failed = false, .max_files = 8, .allocation_unit_size = 16 * 1024,
    };
    auto error = esp_vfs_fat_sdmmc_mount(mount, &host, &slot, &config, &card);
    if (error != ESP_OK) {
        card = nullptr;
        ESP_LOGW(tag, "SD unavailable: %s; continuing without formatting", esp_err_to_name(error));
        return;
    }
    auto status = pxa_board::RegisterSdVolume(mount, card);
    if (status != PXA_STATUS_OK) {
        ESP_LOGW(tag, "SD volume registration failed: %ld", long(status));
        if (esp_vfs_fat_sdcard_unmount(mount, card) == ESP_OK) card = nullptr;
        return;
    }
    ESP_LOGI(tag, "SD ready at %s, SDMMC slot 0, 4-bit 20 MHz", mount);
}
