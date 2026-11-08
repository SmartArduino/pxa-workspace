/* Shipped as source and built by your ESP-IDF, isolating sdmmc_host_t / sdmmc_card_t differences between IDF versions. Do not modify. */
#pragma once

#include <stddef.h>
#include "esp_err.h"
#include "sd_protocol_types.h"

typedef struct {
    int is_sdio;
    int num_io_functions;
    int bus_width;
    int real_freq_khz;
    int max_freq_khz;
} p4w_sdmmc_card_info_t;

size_t p4w_sdmmc_card_size(void);

esp_err_t p4w_sdmmc_slot_init(int clk, int cmd, int d0, int d1, int d2, int d3);

esp_err_t p4w_sdmmc_card_init(sdmmc_card_t *card, int max_freq_khz, int phase);

void p4w_sdmmc_card_get_info(const sdmmc_card_t *card, p4w_sdmmc_card_info_t *info);

esp_err_t p4w_sdmmc_card_boost_clk(sdmmc_card_t *card, int freq_khz, int phase);

void p4w_sdmmc_quiet_wait_int_log(void);
