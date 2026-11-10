// SPDX-License-Identifier: Apache-2.0
#include "espp4_a_audio.h"
#include <algorithm>
#include <esp_codec_dev_defaults.h>
#include <es8311_codec.h>
#include <esp_log.h>

bool Espp4Audio::Check(int result, const char* operation) {
    if (result == ESP_OK) return true;
    ESP_LOGE("espp4_audio", "%s failed: code=%d", operation, result);
    return false;
}

bool Espp4Audio::Initialize() {
    mutex_ = xSemaphoreCreateMutex();
    if (!Check(mutex_ ? ESP_OK : ESP_ERR_NO_MEM, "control mutex")) return false;
    i2c_master_bus_config_t bus_config{};
    bus_config.i2c_port = I2C_NUM_0;
    bus_config.sda_io_num = GPIO_NUM_28; bus_config.scl_io_num = GPIO_NUM_25;
    bus_config.clk_source = I2C_CLK_SRC_DEFAULT; bus_config.glitch_ignore_cnt = 7;
    bus_config.flags.enable_internal_pullup = true;
    i2c_master_bus_handle_t bus;
    if (!Check(i2c_new_master_bus(&bus_config, &bus), "audio I2C")) return false;
    i2s_chan_config_t channels = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channels.dma_desc_num = 3; channels.dma_frame_num = 320; channels.auto_clear = true;
    if (!Check(i2s_new_channel(&channels, &tx_, &rx_), "I2S channels")) return false;
    const i2s_std_config_t standard = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {.mclk = GPIO_NUM_26, .bclk = GPIO_NUM_27, .ws = GPIO_NUM_30,
            .dout = GPIO_NUM_31, .din = GPIO_NUM_29},
    };
    if (!Check(i2s_channel_init_std_mode(tx_, &standard), "I2S TX format") ||
        !Check(i2s_channel_init_std_mode(rx_, &standard), "I2S RX format")) return false;
    audio_codec_i2s_cfg_t data_config = {.port = I2S_NUM_0, .rx_handle = rx_, .tx_handle = tx_};
    auto* data = audio_codec_new_i2s_data(&data_config);
    audio_codec_i2c_cfg_t control_config = {.port = I2C_NUM_0,
        .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = bus};
    es8311_codec_cfg_t config{};
    config.ctrl_if = audio_codec_new_i2c_ctrl(&control_config);
    config.gpio_if = audio_codec_new_gpio();
    config.codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH;
    config.pa_pin = -1; // NS4150 is always enabled on this DevKit.
    config.use_mclk = false; // Match supplier's BCLK-derived ES8311 clock.
    // DevKit v1.1 powers both NS4150 and ES8311 from VDD3V3.
    config.hw_gain = {.pa_voltage = 3.3f, .codec_dac_voltage = 3.3f};
    if (!Check(data && config.ctrl_if && config.gpio_if ? ESP_OK : ESP_ERR_NO_MEM,
               "codec interfaces")) return false;
    const auto* interface = es8311_codec_new(&config);
    if (!Check(interface ? ESP_OK : ESP_FAIL, "ES8311 control")) return false;
    esp_codec_dev_cfg_t device = {.dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
        .codec_if = interface, .data_if = data};
    codec_ = esp_codec_dev_new(&device);
    esp_codec_dev_sample_info_t info{};
    info.bits_per_sample = 16; info.channel = 2; info.sample_rate = 16000;
    if (!Check(codec_ ? ESP_OK : ESP_ERR_NO_MEM, "codec device") ||
        !Check(esp_codec_dev_open(codec_, &info), "codec open")) return false;
    // Leave RX stopped until a permitted microphone resource is opened.
    if (!Check(i2s_channel_disable(rx_), "stop idle microphone") || !SetVolume(volume()) ||
        !Check(esp_codec_dev_set_out_mute(codec_, false), "DAC unmute")) return false;
#if PXA_ESPP4_AUDIO_SELFTEST
    DeviceSelfTest();
#endif
    if (!output_.Initialize([](void* c, const int16_t* pcm, size_t n) {
            return static_cast<Espp4Audio*>(c)->Write(pcm, n);
        }, this, true)) return false;
    if (!capture_.Initialize([](void* c, bool on) {
            return static_cast<Espp4Audio*>(c)->CaptureEnabled(on);
        }, [](void* c, int16_t* pcm, size_t n) {
            return static_cast<Espp4Audio*>(c)->Read(pcm, n);
        }, this)) return false;
    ESP_LOGI("espp4_audio", "ES8311 speaker and permission-controlled microphone: 16 kHz");
    return true;
}

bool Espp4Audio::SetVolume(uint8_t percent) {
    if (!codec_ || !mutex_) return false;
    percent = std::min<uint8_t>(percent, 100);
    xSemaphoreTake(mutex_, portMAX_DELAY);
    const bool ok = Check(esp_codec_dev_set_out_vol(codec_, percent), "DAC volume");
    xSemaphoreGive(mutex_);
    if (ok) volume_.store(percent);
    return ok;
}
bool Espp4Audio::Write(const int16_t* pcm, size_t n) {
    if (!pcm || !n || n > 320) return false;
    unsigned peak = 0;
    if (!signal_logged_) for (size_t i = 0; i < n; ++i)
        peak = std::max(peak, unsigned(pcm[i] < 0 ? -int32_t(pcm[i]) : pcm[i]));
    for (size_t i = 0; i < n; ++i) stereo_[2*i] = stereo_[2*i+1] = pcm[i];
    size_t written = 0;
    const auto result = i2s_channel_write(tx_, stereo_, n * 4, &written, 100);
    const bool ok = result == ESP_OK && written == n * 4;
    if (!ok && !output_failed_)
        ESP_LOGE("espp4_audio", "I2S output failed: code=%d bytes=%u/%u",
            result, unsigned(written), unsigned(n * 4));
    else if (ok && output_failed_) ESP_LOGI("espp4_audio", "I2S output recovered");
    output_failed_ = !ok;
    if (ok && peak && !signal_logged_) {
        signal_logged_ = true;
        ESP_LOGI("espp4_audio", "PCM output active: samples=%u peak=%u volume=%u%%",
            unsigned(n), peak, unsigned(volume()));
    }
    return ok;
}
bool Espp4Audio::CaptureEnabled(bool on) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    esp_err_t result = ESP_OK;
    if (on) result = esp_codec_dev_set_in_gain(codec_, 24.0f);
    if (result == ESP_OK) result = on ? i2s_channel_enable(rx_) : i2s_channel_disable(rx_);
    xSemaphoreGive(mutex_);
    return Check(result, on ? "microphone start" : "microphone stop");
}
int32_t Espp4Audio::Read(int16_t* pcm, size_t n) {
    if (!pcm || !n || n > 320) return PXA_STATUS_INVALID_ARGUMENT;
    size_t bytes = 0;
    const auto result = i2s_channel_read(rx_, input_, n * 4, &bytes, 40);
    if ((result != ESP_OK && result != ESP_ERR_TIMEOUT) || bytes > n*4 || bytes%4)
        return PXA_STATUS_IO_ERROR;
    for (size_t i = 0; i < bytes/4; ++i) pcm[i] = input_[2*i];
    return bytes ? int32_t(bytes/4) : PXA_STATUS_WOULD_BLOCK;
}
