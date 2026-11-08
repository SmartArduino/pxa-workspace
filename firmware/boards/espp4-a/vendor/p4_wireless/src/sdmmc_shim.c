#include "sdmmc_shim.h"

#include "driver/sdmmc_host.h"
#include "driver/sdmmc_default_configs.h"
#include "sdmmc_cmd.h"
#include "esp_log.h"
#include "esp_idf_version.h"

#define P4W_SDMMC_SLOT  SDMMC_HOST_SLOT_1

/* Enum values start at 1 on IDF 6, so a plain cast does not work */
static sdmmc_delay_phase_t phase_to_enum(int phase)
{
    static const sdmmc_delay_phase_t map[] = {
        SDMMC_DELAY_PHASE_0, SDMMC_DELAY_PHASE_1, SDMMC_DELAY_PHASE_2, SDMMC_DELAY_PHASE_3,
    };
    return map[phase];
}

size_t p4w_sdmmc_card_size(void)
{
    return sizeof(sdmmc_card_t);
}

esp_err_t p4w_sdmmc_slot_init(int clk, int cmd, int d0, int d1, int d2, int d3)
{
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();

    slot_config.width = 4;
    slot_config.clk = clk;
    slot_config.cmd = cmd;
    slot_config.d0 = d0;
    slot_config.d1 = d1;
    slot_config.d2 = d2;
    slot_config.d3 = d3;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    return sdmmc_host_init_slot(P4W_SDMMC_SLOT, &slot_config);
}

esp_err_t p4w_sdmmc_card_init(sdmmc_card_t *card, int max_freq_khz, int phase)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();

    host.slot = P4W_SDMMC_SLOT;
    host.max_freq_khz = max_freq_khz;
    host.input_delay_phase = phase_to_enum(phase);
    host.flags = SDMMC_HOST_FLAG_4BIT | SDMMC_HOST_FLAG_ALLOC_ALIGNED_BUF;
    return sdmmc_card_init(&host, card);
}

void p4w_sdmmc_card_get_info(const sdmmc_card_t *card, p4w_sdmmc_card_info_t *info)
{
    info->is_sdio = card->is_sdio;
    info->num_io_functions = card->num_io_functions;
    info->bus_width = 1 << card->log_bus_width;
    info->real_freq_khz = card->real_freq_khz;
    info->max_freq_khz = card->max_freq_khz;
}

esp_err_t p4w_sdmmc_card_boost_clk(sdmmc_card_t *card, int freq_khz, int phase)
{
    int real_khz = 0;
    esp_err_t err = sdmmc_host_set_card_clk(P4W_SDMMC_SLOT, freq_khz);

    if (err != ESP_OK) {
        return err;
    }
    if (phase != 0) {
        sdmmc_host_set_input_delay(P4W_SDMMC_SLOT, phase_to_enum(phase));
    }
    if (sdmmc_host_get_real_freq(P4W_SDMMC_SLOT, &real_khz) == ESP_OK) {
        card->real_freq_khz = real_khz;
        card->max_freq_khz = (uint32_t)real_khz;
    }
    return ESP_OK;
}

void p4w_sdmmc_quiet_wait_int_log(void)
{
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
    /* Interrupt wait timeouts are normal here, but IDF 6 logs each one as an error */
    esp_log_level_set("sdmmc_periph", ESP_LOG_NONE);
#endif
}
