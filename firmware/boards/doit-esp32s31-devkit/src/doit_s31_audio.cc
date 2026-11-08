// SPDX-License-Identifier: Apache-2.0
#include "doit_s31_audio.h"
#include <algorithm>
#include <esp_codec_dev_defaults.h>
#include <es8311_codec.h>
#include <esp_log.h>

bool DoitS31Audio::Initialize(i2c_master_bus_handle_t bus) {
    mutex_ = xSemaphoreCreateMutex();
    if (!mutex_) return false;
    if (!bus) return false;
    i2s_chan_config_t channels = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channels.dma_desc_num = 3; channels.dma_frame_num = 320; channels.auto_clear = true;
    if (i2s_new_channel(&channels, &tx_, &rx_) != ESP_OK) return false;
    const i2s_std_config_t standard = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {.mclk = GPIO_NUM_49, .bclk = GPIO_NUM_48, .ws = GPIO_NUM_46,
            .dout = GPIO_NUM_47, .din = GPIO_NUM_45},
    };
    if (i2s_channel_init_std_mode(tx_, &standard) != ESP_OK ||
        i2s_channel_init_std_mode(rx_, &standard) != ESP_OK) return false;
    audio_codec_i2s_cfg_t data_config = {.port = I2S_NUM_0, .rx_handle = rx_, .tx_handle = tx_};
    auto* data = audio_codec_new_i2s_data(&data_config);
    audio_codec_i2c_cfg_t control_config = {.port = I2C_NUM_0,
        .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = bus};
    es8311_codec_cfg_t config{};
    config.ctrl_if = audio_codec_new_i2c_ctrl(&control_config);
    config.gpio_if = audio_codec_new_gpio();
    config.codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH;
    config.pa_pin = 39; // NS4150 CTRL, active high.
    config.use_mclk = true; // ES_MCLK is wired to GPIO49.
    config.hw_gain = {.pa_voltage = 5.0f, .codec_dac_voltage = 3.3f};
    if (!data || !config.ctrl_if || !config.gpio_if) return false;
    const auto* interface = es8311_codec_new(&config);
    if (!interface) return false;
    esp_codec_dev_cfg_t device = {.dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
        .codec_if = interface, .data_if = data};
    codec_ = esp_codec_dev_new(&device);
    esp_codec_dev_sample_info_t info{};
    info.bits_per_sample = 16; info.channel = 2; info.sample_rate = 16000;
    if (!codec_ || esp_codec_dev_open(codec_, &info) != ESP_OK) return false;
    // Leave RX stopped until a permitted microphone resource is opened.
    if (i2s_channel_disable(rx_) != ESP_OK || !SetVolume(volume())) return false;
    if (!output_.Initialize([](void* c, const int16_t* pcm, size_t n) {
            return static_cast<DoitS31Audio*>(c)->Write(pcm, n);
        }, this, true)) return false;
    if (!capture_.Initialize([](void* c, bool on) {
            return static_cast<DoitS31Audio*>(c)->CaptureEnabled(on);
        }, [](void* c, int16_t* pcm, size_t n) {
            return static_cast<DoitS31Audio*>(c)->Read(pcm, n);
        }, this)) return false;
    ESP_LOGI("doit_s31_audio", "ES8311 speaker and permission-controlled microphone: 16 kHz");
    return true;
}

bool DoitS31Audio::SetVolume(uint8_t percent) {
    if (!codec_ || !mutex_) return false;
    percent = std::min<uint8_t>(percent, 100);
    xSemaphoreTake(mutex_, portMAX_DELAY);
    const bool ok = esp_codec_dev_set_out_vol(codec_, percent) == ESP_OK;
    xSemaphoreGive(mutex_);
    if (ok) volume_.store(percent);
    return ok;
}
bool DoitS31Audio::Write(const int16_t* pcm, size_t n) {
    if (!pcm || !n || n > 320) return false;
    for (size_t i = 0; i < n; ++i) stereo_[2*i] = stereo_[2*i+1] = pcm[i];
    size_t written = 0;
    return i2s_channel_write(tx_, stereo_, n * 4, &written, 100) == ESP_OK && written == n * 4;
}
bool DoitS31Audio::CaptureEnabled(bool on) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    esp_err_t result = ESP_OK;
    if (on) result = esp_codec_dev_set_in_gain(codec_, 24.0f);
    if (result == ESP_OK) result = on ? i2s_channel_enable(rx_) : i2s_channel_disable(rx_);
    xSemaphoreGive(mutex_);
    return result == ESP_OK;
}
int32_t DoitS31Audio::Read(int16_t* pcm, size_t n) {
    if (!pcm || !n || n > 320) return PXA_STATUS_INVALID_ARGUMENT;
    size_t bytes = 0;
    const auto result = i2s_channel_read(rx_, input_, n * 4, &bytes, 40);
    if ((result != ESP_OK && result != ESP_ERR_TIMEOUT) || bytes > n*4 || bytes%4)
        return PXA_STATUS_IO_ERROR;
    for (size_t i = 0; i < bytes/4; ++i) pcm[i] = input_[2*i];
    return bytes ? int32_t(bytes/4) : PXA_STATUS_WOULD_BLOCK;
}
