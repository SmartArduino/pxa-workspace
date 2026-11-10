// SPDX-License-Identifier: Apache-2.0
#include "espp4_a_sd.h"
#include <driver/gpio.h>
#include <driver/sdmmc_host.h>
#include <driver/uart.h>
#include <esp_log.h>
#include <esp_vfs_fat.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sd_protocol_defs.h>
#include <algorithm>
#include <cstdio>

namespace {
size_t chunk_blocks = 1;
int preerase = 0;
sdmmc_card_t* mounted_card = nullptr;

esp_err_t transaction(int slot, sdmmc_command_t* command) {
    if (preerase && mounted_card && command->opcode == MMC_WRITE_BLOCK_MULTIPLE) {
        sdmmc_command_t prefix{};
        prefix.opcode = MMC_APP_CMD; prefix.arg = uint32_t(mounted_card->rca) << 16;
        prefix.flags = SCF_CMD_AC | SCF_RSP_R1; prefix.timeout_ms = 1000;
        auto result = sdmmc_host_do_transaction(slot, &prefix);
        if (result != ESP_OK || prefix.error != ESP_OK) return result == ESP_OK ? prefix.error : result;
        sdmmc_command_t erase{};
        erase.opcode = 23; erase.arg = command->datalen / command->blklen;
        erase.flags = SCF_CMD_AC | SCF_RSP_R1; erase.timeout_ms = 1000;
        result = sdmmc_host_do_transaction(slot, &erase);
        if (result != ESP_OK || erase.error != ESP_OK) return result == ESP_OK ? erase.error : result;
    }
    if (command->opcode == MMC_WRITE_BLOCK_SINGLE || command->opcode == MMC_WRITE_BLOCK_MULTIPLE)
        command->flags |= SCF_WAIT_BUSY;
    const auto result = sdmmc_host_do_transaction(slot, command);
    if ((result != ESP_OK || command->error != ESP_OK) && command->opcode != 5 && command->opcode != 52)
        ESP_LOGE("sd_probe", "CMD%lu bytes=%lu response=%08lx error=%s", (unsigned long)command->opcode,
            (unsigned long)command->datalen, (unsigned long)command->response[0], esp_err_to_name(result != ESP_OK ? result : command->error));
    return result;
}

void run(int width, int khz, int drive, int blocks, int clock_on, int erase) {
    if ((width != 1 && width != 4) || khz < 400 || khz > 40000 || drive < 0 || drive > 3 || blocks < 1 || blocks > 32)
        return;
    chunk_blocks = blocks; preerase = erase; mounted_card = nullptr;
    ESP_LOGI("sd_probe", "BEGIN width=%d khz=%d drive=%d blocks=%d clock_on=%d preerase=%d", width, khz, drive, blocks, clock_on, erase);
    gpio_set_level(GPIO_NUM_4, 1);
    gpio_config_t power{}; power.pin_bit_mask = 1ULL << 4; power.mode = GPIO_MODE_OUTPUT;
    ESP_ERROR_CHECK(gpio_config(&power));
    gpio_config_t discharge{}; discharge.mode = GPIO_MODE_OUTPUT;
    for (int pin = 39; pin <= 44; ++pin) {
        gpio_set_level(gpio_num_t(pin), 0); discharge.pin_bit_mask |= 1ULL << pin;
    }
    ESP_ERROR_CHECK(gpio_config(&discharge));
    vTaskDelay(pdMS_TO_TICKS(2000));
    discharge.mode = GPIO_MODE_INPUT; ESP_ERROR_CHECK(gpio_config(&discharge));
    ESP_ERROR_CHECK(gpio_set_level(GPIO_NUM_4, 0));
    vTaskDelay(pdMS_TO_TICKS(50));
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0; host.max_freq_khz = khz;
    host.flags = SDMMC_HOST_FLAG_1BIT | SDMMC_HOST_FLAG_DEINIT_ARG;
    if (width == 4) host.flags |= SDMMC_HOST_FLAG_4BIT;
    host.do_transaction = transaction;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = width; slot.clk = GPIO_NUM_43; slot.cmd = GPIO_NUM_44;
    slot.d0 = GPIO_NUM_39; slot.d1 = GPIO_NUM_40; slot.d2 = GPIO_NUM_41; slot.d3 = GPIO_NUM_42;
    slot.cd = GPIO_NUM_45; slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    esp_vfs_fat_sdmmc_mount_config_t mount{};
    mount.format_if_mount_failed = false; mount.max_files = 4; mount.allocation_unit_size = 16384;
    sdmmc_card_t* card = nullptr;
    const auto result = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot, &mount, &card);
    ESP_LOGI("sd_probe", "mount: %s", esp_err_to_name(result));
    if (result == ESP_OK) {
        mounted_card = card;
        sdmmc_card_print_info(stdout, card);
        for (int pin = 39; pin <= 44; ++pin)
            ESP_ERROR_CHECK(gpio_set_drive_capability(gpio_num_t(pin), gpio_drive_cap_t(drive)));
        if (clock_on) ESP_ERROR_CHECK(sdmmc_host_set_cclk_always_on(0, true));
        espp4_a_sd_device_test("/sdcard");
        ESP_LOGI("sd_probe", "unmount: %s", esp_err_to_name(esp_vfs_fat_sdcard_unmount("/sdcard", card)));
        mounted_card = nullptr;
    }
    ESP_LOGI("sd_probe", "READY: T width khz drive blocks clock_on preerase");
}
}

extern "C" {
esp_err_t __real_sdmmc_read_sectors(sdmmc_card_t*, void*, size_t, size_t);
esp_err_t __real_sdmmc_write_sectors(sdmmc_card_t*, const void*, size_t, size_t);
esp_err_t __wrap_sdmmc_read_sectors(sdmmc_card_t* card, void* data, size_t start, size_t count) {
    for (size_t offset = 0; offset < count;) {
        const size_t blocks = std::min(chunk_blocks, count - offset);
        const auto result = __real_sdmmc_read_sectors(card, static_cast<uint8_t*>(data) + offset * card->csd.sector_size, start + offset, blocks);
        if (result != ESP_OK) return result;
        offset += blocks;
    }
    return ESP_OK;
}
esp_err_t __wrap_sdmmc_write_sectors(sdmmc_card_t* card, const void* data, size_t start, size_t count) {
    for (size_t offset = 0; offset < count;) {
        const size_t blocks = std::min(chunk_blocks, count - offset);
        const auto result = __real_sdmmc_write_sectors(card, static_cast<const uint8_t*>(data) + offset * card->csd.sector_size, start + offset, blocks);
        if (result != ESP_OK) return result;
        offset += blocks;
    }
    return ESP_OK;
}
void app_main() {
    uart_config_t uart{};
    uart.baud_rate = 2000000; uart.data_bits = UART_DATA_8_BITS;
    uart.parity = UART_PARITY_DISABLE; uart.stop_bits = UART_STOP_BITS_1;
    uart.source_clk = UART_SCLK_DEFAULT;
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_0, &uart));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_0, 37, 38, -1, -1));
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 1024, 0, 0, nullptr, 0));
    run(4, 2500, 1, 32, 0, 0);
    char line[96];
    size_t used = 0;
    for (;;) {
        char ch;
        if (uart_read_bytes(UART_NUM_0, &ch, 1, pdMS_TO_TICKS(100)) != 1) continue;
        if (ch != '\n' && ch != '\r') {
            if (used + 1 < sizeof(line)) line[used++] = ch;
            continue;
        }
        line[used] = 0; used = 0;
        int width, khz, drive, blocks, clock_on, erase;
        if (std::sscanf(line, "T %d %d %d %d %d %d", &width, &khz, &drive, &blocks, &clock_on, &erase) == 6)
            run(width, khz, drive, blocks, clock_on, erase);
    }
}
}
