#include "esp32s31_korvo_1_audio.h"

#include "esp32s31_korvo_1_config.h"

#include <esp_codec_dev_defaults.h>
#include <esp_log.h>
#include <es8389_codec.h>
#include <pxa/pxa_host.h>

namespace {
constexpr char kTag[] = "korvo_audio";
}

bool Esp32S31Korvo1Audio::Initialize(i2c_master_bus_handle_t i2c_bus) {
    mutex_ = xSemaphoreCreateMutex();
    if (mutex_ == nullptr) return false;

    i2s_chan_config_t channel_config = {};
    channel_config.id = KORVO_AUDIO_I2S_PORT;
    channel_config.role = I2S_ROLE_MASTER;
    channel_config.dma_desc_num = 6;
    channel_config.dma_frame_num = 240;
    channel_config.auto_clear = true;
    if (i2s_new_channel(&channel_config, &tx_channel_, &rx_channel_) != ESP_OK) {
        ESP_LOGE(kTag, "Cannot create I2S0 channels");
        return false;
    }
    const i2s_std_config_t std_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(KORVO_AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = KORVO_AUDIO_MCLK,
            .bclk = KORVO_AUDIO_BCLK,
            .ws = KORVO_AUDIO_WS,
            .dout = KORVO_AUDIO_DOUT,
            .din = KORVO_AUDIO_DIN,
        },
    };
    if (i2s_channel_init_std_mode(tx_channel_, &std_config) != ESP_OK ||
        i2s_channel_init_std_mode(rx_channel_, &std_config) != ESP_OK ||
        i2s_channel_enable(tx_channel_) != ESP_OK ||
        i2s_channel_enable(rx_channel_) != ESP_OK) {
        ESP_LOGE(kTag, "Cannot initialize I2S0");
        return false;
    }

    audio_codec_i2s_cfg_t i2s_config = {
        .port = KORVO_AUDIO_I2S_PORT,
        .rx_handle = rx_channel_,
        .tx_handle = tx_channel_,
    };
    data_if_ = audio_codec_new_i2s_data(&i2s_config);
    const audio_codec_gpio_if_t* gpio_if = audio_codec_new_gpio();
    audio_codec_i2c_cfg_t i2c_config = {
        .port = static_cast<uint8_t>(KORVO_I2C_PORT),
        .addr = ES8389_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c_bus,
    };
    const audio_codec_ctrl_if_t* ctrl_if = audio_codec_new_i2c_ctrl(&i2c_config);
    if (data_if_ == nullptr || gpio_if == nullptr || ctrl_if == nullptr) return false;

    es8389_codec_cfg_t codec_config = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = KORVO_AUDIO_PA,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = false,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = {.pa_voltage = 5.0f, .codec_dac_voltage = 3.3f},
        .no_dac_ref = false,
    };
    const audio_codec_if_t* codec_if = es8389_codec_new(&codec_config);
    if (codec_if == nullptr) return false;
    esp_codec_dev_cfg_t device_config = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = codec_if,
        .data_if = data_if_,
    };
    speaker_ = esp_codec_dev_new(&device_config);
    if (speaker_ == nullptr) return false;
    esp_codec_dev_sample_info_t sample_info = {
        .bits_per_sample = 16,
        .channel = KORVO_AUDIO_CHANNELS,
        .channel_mask = 0,
        .sample_rate = KORVO_AUDIO_SAMPLE_RATE,
        .mclk_multiple = 0,
    };
    if (esp_codec_dev_open(speaker_, &sample_info) != ESP_OK) return false;
    SetVolume(volume());
    pxa_host_set_audio_sink(Submit, Flush, this);
    ESP_LOGI(kTag, "ES8389 speaker ready at %d Hz stereo", KORVO_AUDIO_SAMPLE_RATE);
    return true;
}

void Esp32S31Korvo1Audio::SetVolume(uint8_t percent) {
    if (percent > 100) percent = 100;
    volume_.store(percent);
    if (speaker_ == nullptr || mutex_ == nullptr) return;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    (void)esp_codec_dev_set_out_vol(speaker_, percent);
    xSemaphoreGive(mutex_);
}

bool Esp32S31Korvo1Audio::Submit(void* context, uint8_t voice,
                                  const int16_t* pcm, size_t samples) {
    (void)voice;
    auto* self = static_cast<Esp32S31Korvo1Audio*>(context);
    return self != nullptr && pcm != nullptr && samples != 0 && self->Write(pcm, samples);
}

void Esp32S31Korvo1Audio::Flush(void* context, uint8_t voice) {
    (void)context;
    (void)voice;
}

bool Esp32S31Korvo1Audio::Write(const int16_t* pcm, size_t samples) {
    if (speaker_ == nullptr || mutex_ == nullptr) return false;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    const esp_err_t result = esp_codec_dev_write(
        speaker_, const_cast<int16_t*>(pcm), samples * sizeof(*pcm));
    xSemaphoreGive(mutex_);
    return result == ESP_OK;
}
