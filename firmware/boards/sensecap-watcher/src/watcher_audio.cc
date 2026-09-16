#include "watcher_audio.h"

#include "sensecap_watcher_config.h"

#include <cstring>

#include <esp_codec_dev_defaults.h>
#include <esp_log.h>
#include <es8311_codec.h>
#include <pxa/pxa_host.h>

namespace {
constexpr char kTag[] = "WatcherAudio";
// The factory firmware caps the codec volume below 100 to avoid clipping.
constexpr int kMaxCodecVolume = 95;
}  // namespace

bool WatcherAudio::Initialize(i2c_master_bus_handle_t i2c_bus) {
    mutex_ = xSemaphoreCreateMutex();
    if (mutex_ == nullptr) return false;

    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(
        WATCHER_AUDIO_I2S_PORT, I2S_ROLE_MASTER);
    channel_config.auto_clear = true;
    if (i2s_new_channel(&channel_config, &tx_channel_, nullptr) != ESP_OK) {
        tx_channel_ = nullptr;
        ESP_LOGE(kTag, "Cannot create I2S TX channel");
        return false;
    }

    const i2s_std_config_t std_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(WATCHER_AUDIO_SAMPLE_RATE),
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_MONO,
            .slot_mask = I2S_STD_SLOT_BOTH,
            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = true,
            .big_endian = false,
            .bit_order_lsb = false,
        },
        .gpio_cfg = {
            .mclk = WATCHER_AUDIO_MCLK,
            .bclk = WATCHER_AUDIO_SCLK,
            .ws = WATCHER_AUDIO_LRCK,
            .dout = WATCHER_AUDIO_DOUT,
            .din = WATCHER_AUDIO_DSIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    if (i2s_channel_init_std_mode(tx_channel_, &std_config) != ESP_OK ||
        i2s_channel_enable(tx_channel_) != ESP_OK) {
        ESP_LOGE(kTag, "Cannot start I2S0 TX");
        return false;
    }

    audio_codec_i2s_cfg_t i2s_config = {};
    i2s_config.port = WATCHER_AUDIO_I2S_PORT;
    i2s_config.rx_handle = nullptr;
    i2s_config.tx_handle = tx_channel_;
    data_if_ = audio_codec_new_i2s_data(&i2s_config);
    if (data_if_ == nullptr) {
        ESP_LOGE(kTag, "Cannot create codec I2S data interface");
        return false;
    }

    const audio_codec_gpio_if_t* gpio_if = audio_codec_new_gpio();
    if (gpio_if == nullptr) return false;
    audio_codec_i2c_cfg_t i2c_config = {};
    i2c_config.port = static_cast<uint8_t>(WATCHER_I2C_PORT);
    i2c_config.addr = WATCHER_AUDIO_ES8311_ADDRESS;
    i2c_config.bus_handle = i2c_bus;
    const audio_codec_ctrl_if_t* ctrl_if = audio_codec_new_i2c_ctrl(
        &i2c_config);
    if (ctrl_if == nullptr) {
        ESP_LOGE(kTag, "Cannot create codec I2C control interface");
        return false;
    }

    const esp_codec_dev_hw_gain_t hardware_gain = {
        .pa_voltage = 5.0,
        .codec_dac_voltage = 3.3,
    };
    es8311_codec_cfg_t codec_config = {};
    codec_config.ctrl_if = ctrl_if;
    codec_config.gpio_if = gpio_if;
    codec_config.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;
    codec_config.pa_pin = GPIO_NUM_NC;
    codec_config.pa_reverted = false;
    codec_config.master_mode = false;
    codec_config.use_mclk = true;
    codec_config.digital_mic = false;
    codec_config.invert_mclk = false;
    codec_config.invert_sclk = false;
    codec_config.hw_gain = hardware_gain;
    const audio_codec_if_t* codec_if = es8311_codec_new(&codec_config);
    if (codec_if == nullptr) {
        ESP_LOGE(kTag, "Cannot create ES8311 codec interface");
        return false;
    }

    esp_codec_dev_cfg_t device_config = {};
    device_config.dev_type = ESP_CODEC_DEV_TYPE_OUT;
    device_config.codec_if = codec_if;
    device_config.data_if = data_if_;
    speaker_ = esp_codec_dev_new(&device_config);
    if (speaker_ == nullptr) {
        ESP_LOGE(kTag, "Cannot create codec device");
        return false;
    }

    esp_codec_dev_sample_info_t sample_info = {};
    sample_info.bits_per_sample = 16;
    sample_info.channel = WATCHER_AUDIO_CHANNELS;
    sample_info.channel_mask = 0;
    sample_info.sample_rate = WATCHER_AUDIO_SAMPLE_RATE;
    sample_info.mclk_multiple = 0;
    if (esp_codec_dev_open(speaker_, &sample_info) != ESP_OK) {
        ESP_LOGE(kTag, "Cannot open ES8311 playback");
        return false;
    }
    SetVolume(volume_.load());
    pxa_host_set_audio_sink(Submit, Flush, this);
    ESP_LOGI(kTag, "ES8311 speaker ready at %d Hz mono",
             WATCHER_AUDIO_SAMPLE_RATE);
    return true;
}

void WatcherAudio::SetVolume(uint8_t percent) {
    if (percent > 100) percent = 100;
    volume_.store(percent);
    if (speaker_ == nullptr || mutex_ == nullptr) return;
    const int codec_volume =
        percent > kMaxCodecVolume ? kMaxCodecVolume : percent;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    esp_codec_dev_set_out_vol(speaker_, codec_volume);
    xSemaphoreGive(mutex_);
}

bool WatcherAudio::Submit(void* context, uint8_t voice, const int16_t* pcm,
                          size_t samples) {
    (void)voice;
    auto* self = static_cast<WatcherAudio*>(context);
    if (self == nullptr || pcm == nullptr || samples == 0) return false;
    return self->Write(pcm, samples);
}

void WatcherAudio::Flush(void* context, uint8_t voice) {
    (void)context;
    (void)voice;
}

bool WatcherAudio::Write(const int16_t* pcm, size_t samples) {
    if (speaker_ == nullptr || mutex_ == nullptr) return false;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    const esp_err_t result = esp_codec_dev_write(
        speaker_, const_cast<int16_t*>(pcm), samples * sizeof(int16_t));
    xSemaphoreGive(mutex_);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "Playback write failed: %s", esp_err_to_name(result));
        return false;
    }
    return true;
}
