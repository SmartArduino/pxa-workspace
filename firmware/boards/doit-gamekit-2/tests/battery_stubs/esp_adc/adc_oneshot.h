#pragma once
#include <cstdint>

using esp_err_t = int;
inline constexpr esp_err_t ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_TIMEOUT = 1;
inline const char* esp_err_to_name(esp_err_t) { return "test error"; }
enum adc_unit_t { ADC_UNIT_1, ADC_UNIT_2 };
enum adc_channel_t { ADC_CHANNEL_7 = 7 };
enum adc_atten_t { ADC_ATTEN_DB_0 };
enum adc_bitwidth_t { ADC_BITWIDTH_DEFAULT };
using adc_oneshot_unit_handle_t = void*;
struct adc_oneshot_unit_init_cfg_t { adc_unit_t unit_id; };
struct adc_oneshot_chan_cfg_t { adc_atten_t atten; adc_bitwidth_t bitwidth; };
esp_err_t adc_oneshot_io_to_channel(int, adc_unit_t*, adc_channel_t*);
esp_err_t adc_oneshot_new_unit(const adc_oneshot_unit_init_cfg_t*, adc_oneshot_unit_handle_t*);
esp_err_t adc_oneshot_config_channel(adc_oneshot_unit_handle_t, adc_channel_t, const adc_oneshot_chan_cfg_t*);
esp_err_t adc_oneshot_del_unit(adc_oneshot_unit_handle_t);
esp_err_t adc_oneshot_read(adc_oneshot_unit_handle_t, adc_channel_t, int*);
