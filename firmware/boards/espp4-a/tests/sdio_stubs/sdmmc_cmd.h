#pragma once
#include <cstddef>
#include <cstdint>
using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_ERR_INVALID_ARG = 1, ESP_ERR_INVALID_SIZE = 2, ESP_ERR_NO_MEM = 3;
struct sdmmc_card_t {
    struct { int slot; bool (*check_buffer_alignment)(int, const void*, size_t); } host;
};
