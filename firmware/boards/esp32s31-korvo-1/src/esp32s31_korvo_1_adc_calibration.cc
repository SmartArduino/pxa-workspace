/* SPDX-License-Identifier: Apache-2.0 */

#include "esp32s31_korvo_1_adc_calibration.h"

#include <algorithm>
#include <cinttypes>
#include <cstdlib>
#include <iterator>

#include <esp_check.h>
#include <esp_log.h>
#include <soc/soc_caps.h>

namespace {
constexpr char kTag[] = "korvo_adc";
constexpr uint8_t kSar1Base = 0x10;
constexpr uint8_t kSarHostId = 0;
constexpr uint8_t kRegCalControl = 0x0;
constexpr uint8_t kRegCalSelect = 0x2;
constexpr uint8_t kRegCalSelect1 = 0x3;
constexpr uint8_t kRegRawControl = 0x4;
constexpr int kSampleCount = 256;
constexpr int kCalibrationCycles = 5;
constexpr int kWeightQ = 8;
constexpr int kWeightScale = 1 << kWeightQ;
constexpr int kMaxMillivolts = 2000;
constexpr int kBitCount = 17;

struct CalibrationStep {
    uint8_t bit;
    uint8_t select;
    uint8_t select1;
};
constexpr CalibrationStep kSteps[] = {
    {10, 0x02, 0x01}, {11, 0x04, 0x03}, {12, 0x08, 0x07},
    {13, 0x10, 0x0F}, {14, 0x20, 0x1F}, {15, 0x40, 0x3F},
    {16, 0x80, 0x7F},
};
constexpr int32_t kIdealWeights[kBitCount] = {
    2048, 1024, 512, 256, 256, 128, 64, 32, 32,
    16, 8, 8, 4, 2, 2, 0, 1,
};

struct Calibration {
    bool ready = false;
    int32_t weights[kBitCount] = {};
};
Calibration kCalibration[SOC_ADC_PERIPH_NUM];

extern "C" void regi2c_ctrl_write_reg_mask(uint8_t block, uint8_t host_id,
                                               uint8_t reg, uint8_t msb,
                                               uint8_t lsb, uint8_t value);

void WriteMask(uint8_t reg, uint8_t msb, uint8_t lsb, uint8_t value) {
    regi2c_ctrl_write_reg_mask(kSar1Base, kSarHostId, reg, msb, lsb, value);
}

void SetCalibrationMode(bool done) { WriteMask(kRegCalControl, 0, 0, done); }
void SetRawData(bool enabled) { WriteMask(kRegRawControl, 4, 4, enabled); }
void SetPolarity(bool polarity) { WriteMask(kRegCalControl, 1, 1, polarity); }
void SetStep(const CalibrationStep& step) {
    WriteMask(kRegCalSelect, 7, 0, step.select);
    WriteMask(kRegCalSelect1, 7, 0, step.select1);
}

int32_t CodeQ(const int32_t weights[kBitCount], uint32_t raw) {
    int32_t code = 0;
    raw &= 0x1FFFF;
    for (int bit = 0; bit < kBitCount; ++bit) {
        if (raw & (1U << (kBitCount - 1 - bit))) code += weights[bit];
    }
    return code;
}
}

esp_err_t esp32s31_korvo_1_adc_calibration_init(
    adc_oneshot_unit_handle_t handle, adc_unit_t unit, adc_channel_t channel) {
    ESP_RETURN_ON_FALSE(handle != nullptr && unit == ADC_UNIT_1,
                        ESP_ERR_INVALID_ARG, kTag, "only ADC1 is supported");
    Calibration& calibration = kCalibration[unit];
    if (calibration.ready) {
        SetRawData(true);
        SetCalibrationMode(true);
        return ESP_OK;
    }

    auto* samples = static_cast<uint32_t (*)[2][kSampleCount]>(
        calloc(std::size(kSteps), sizeof(uint32_t[2][kSampleCount])));
    ESP_RETURN_ON_FALSE(samples != nullptr, ESP_ERR_NO_MEM, kTag,
                        "cannot allocate calibration samples");
    for (int bit = 0; bit < kBitCount; ++bit)
        calibration.weights[bit] = kIdealWeights[bit] * kWeightScale;

    SetRawData(true);
    SetCalibrationMode(false);
    esp_err_t result = ESP_OK;
    for (size_t step_index = 0; step_index < std::size(kSteps); ++step_index) {
        SetStep(kSteps[step_index]);
        for (int polarity = 0; polarity < 2; ++polarity) {
            SetPolarity(polarity == 0);
            for (int sample = 0; sample < kSampleCount; ++sample) {
                int raw = 0;
                result = adc_oneshot_read(handle, channel, &raw);
                if (result != ESP_OK) break;
                samples[step_index][polarity][sample] = raw & 0x1FFFF;
            }
            if (result != ESP_OK) break;
        }
        if (result != ESP_OK) break;
    }
    SetCalibrationMode(true);
    if (result != ESP_OK) {
        free(samples);
        return result;
    }

    for (int cycle = 0; cycle < kCalibrationCycles; ++cycle) {
        for (size_t step_index = 0; step_index < std::size(kSteps); ++step_index) {
            int64_t sums[2] = {};
            for (int polarity = 0; polarity < 2; ++polarity) {
                for (int sample = 0; sample < kSampleCount; ++sample)
                    sums[polarity] += CodeQ(calibration.weights,
                                             samples[step_index][polarity][sample]);
            }
            const int32_t delta = static_cast<int32_t>(
                ((sums[0] / kSampleCount) - (sums[1] / kSampleCount)) / 2);
            calibration.weights[kBitCount - 1 - kSteps[step_index].bit] += delta;
        }
    }
    calibration.ready = true;
    free(samples);
    ESP_LOGI(kTag, "GPIO42 ADC calibration completed");
    return ESP_OK;
}

esp_err_t esp32s31_korvo_1_adc_raw_to_mv(adc_unit_t unit, int raw,
                                          int* voltage_mv) {
    ESP_RETURN_ON_FALSE(unit == ADC_UNIT_1 && voltage_mv != nullptr,
                        ESP_ERR_INVALID_ARG, kTag, "invalid ADC conversion");
    const Calibration& calibration = kCalibration[unit];
    ESP_RETURN_ON_FALSE(calibration.ready, ESP_ERR_INVALID_STATE, kTag,
                        "ADC is not calibrated");
    int64_t millivolts = kMaxMillivolts -
        (static_cast<int64_t>(4000) * CodeQ(calibration.weights, raw)) /
            (4393 * kWeightScale);
    millivolts = std::clamp<int64_t>(millivolts, 0, kMaxMillivolts);
    *voltage_mv = static_cast<int>(millivolts);
    return ESP_OK;
}
