// SPDX-License-Identifier: Apache-2.0
#include "espp4_a_audio.h"
#include <esp_log.h>
#include <esp_timer.h>
#include <cmath>
#include <initializer_list>

// Optional native hardware diagnostic, run before registering Host producers.
// It reads only the ES8311's right-hand DAC reference. Microphone samples are
// discarded and never retained or exposed to an app. Normal builds omit it.
void Espp4Audio::DeviceSelfTest() {
    constexpr int16_t wave[] = {0,2296,4243,5543,6000,5543,4243,2296,
        0,-2296,-4243,-5543,-6000,-5543,-4243,-2296};
    int16_t pcm[320];
    bool passed = true;
    for (unsigned i = 0; i < 320; ++i) pcm[i] = wave[i % 16];
    for (int reg : {0x00,0x01,0x02,0x09,0x0a,0x12,0x14,0x31,0x32}) {
        int value = -1;
        const int error = esp_codec_dev_read_reg(codec_, reg, &value);
        ESP_LOGI("espp4_audio_test", "register=%02x value=%02x result=%d", reg, value, error);
        passed = passed && error == ESP_OK;
    }
    for (unsigned phase = 0; phase < 3; ++phase) {
        const bool capture = phase == 1;
        const auto enable = capture ? i2s_channel_enable(rx_) : ESP_OK;
        unsigned writes = 0, reads = 0;
        uint64_t energy = 0, samples = 0;
        double in_phase = 0, quadrature = 0;
        const int64_t start = esp_timer_get_time();
        for (unsigned frame = 0; frame < (capture ? 80u : 40u); ++frame) {
            if (Write(pcm, 320)) ++writes;
            if (capture && enable == ESP_OK) {
                size_t bytes = 0;
                if (i2s_channel_read(rx_, input_, sizeof(input_), &bytes, 50) == ESP_OK) {
                    ++reads;
                    if (frame < 10) continue;
                    for (unsigned i = 0; i < bytes / 4; ++i) {
                        const int32_t sample = input_[2*i+1];
                        energy += int64_t(sample) * sample;
                        in_phase += sample * std::sin(2 * 3.141592653589793 * (samples % 16) / 16);
                        quadrature += sample * std::cos(2 * 3.141592653589793 * (samples % 16) / 16);
                        ++samples;
                    }
                }
            }
        }
        const double rms = samples ? std::sqrt(double(energy) / samples) : 0;
        const double tone_percent = energy ? 200 * (in_phase * in_phase + quadrature * quadrature) / (samples * double(energy)) : 0;
        ESP_LOGI("espp4_audio_test", "phase=%u rx=%d enable=%d writes=%u reads=%u samples=%llu rms=%.1f tone_1khz_percent=%.1f elapsed_us=%lld",
            phase, capture, enable, writes, reads, samples, rms, tone_percent, esp_timer_get_time() - start);
        passed = passed && writes == (capture ? 80u : 40u);
        if (capture) passed = passed && reads == 80 && samples && rms > 1000 && tone_percent > 90;
        if (capture && enable == ESP_OK) passed = (i2s_channel_disable(rx_) == ESP_OK) && passed;
    }
    for (auto& sample : pcm) sample = 0;
    for (unsigned i = 0; i < 4; ++i) (void)Write(pcm, 320);
    ESP_LOGI("espp4_audio_test", "Digital DAC selftest %s; confirm the external speaker by listening",
        passed ? "PASS" : "FAIL");
}
