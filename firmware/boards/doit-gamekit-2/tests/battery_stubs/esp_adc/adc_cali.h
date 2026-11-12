#pragma once
#include "adc_oneshot.h"
using adc_cali_handle_t = void*;
// Deliberately omit adc_cali_raw_to_voltage: it is unsupported on S31.
esp_err_t adc_cali_chan_raw_to_voltage(adc_cali_handle_t, adc_channel_t, int, int*);
