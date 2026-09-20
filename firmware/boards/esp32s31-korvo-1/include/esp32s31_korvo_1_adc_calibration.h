/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <esp_adc/adc_oneshot.h>
#include <esp_err.h>

esp_err_t esp32s31_korvo_1_adc_calibration_init(
    adc_oneshot_unit_handle_t handle, adc_unit_t unit, adc_channel_t channel);
esp_err_t esp32s31_korvo_1_adc_raw_to_mv(adc_unit_t unit, int raw,
                                          int* voltage_mv);
