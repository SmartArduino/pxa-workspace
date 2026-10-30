#pragma once
#include "adc_cali.h"
struct adc_cali_line_fitting_config_t {
    adc_unit_t unit_id;
    adc_atten_t atten;
    adc_bitwidth_t bitwidth;
};
esp_err_t adc_cali_create_scheme_line_fitting(const adc_cali_line_fitting_config_t*, adc_cali_handle_t*);
esp_err_t adc_cali_delete_scheme_line_fitting(adc_cali_handle_t);
