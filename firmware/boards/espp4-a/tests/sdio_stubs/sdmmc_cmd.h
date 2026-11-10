#pragma once
#include <cstddef>
#include <cstdint>
using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_ERR_INVALID_ARG = 1, ESP_ERR_INVALID_SIZE = 2, ESP_ERR_NO_MEM = 3;
constexpr int MMC_WRITE_BLOCK_SINGLE = 24, MMC_WRITE_BLOCK_MULTIPLE = 25;
constexpr int SCF_WAIT_BUSY = 0x2000;
constexpr uint32_t SD_OCR_SDHC_CAP = 1u << 30;
constexpr int SDMMC_FREQ_PROBING = 400;
struct sdmmc_command_t { int opcode; int flags; unsigned timeout_ms; int error; };
struct sdmmc_host_t {
    int slot; bool (*check_buffer_alignment)(int, const void*, size_t);
    esp_err_t (*set_card_clk)(int, uint32_t) = nullptr;
    esp_err_t (*get_real_freq)(int, int*) = nullptr;
};
struct sdmmc_card_t {
    sdmmc_host_t host;
    struct { size_t sector_size = 512, capacity = 100000; } csd{};
    uint32_t ocr = SD_OCR_SDHC_CAP;
};
