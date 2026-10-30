#include "esp32s31_korvo_1_audio.h"

#include "esp32s31_korvo_1_config.h"

#include <esp_codec_dev_defaults.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <es8389_codec.h>

namespace {
constexpr char kTag[] = "korvo_audio";
}

bool Esp32S31Korvo1Audio::Initialize(i2c_master_bus_handle_t i2c_bus) {
    mutex_ = xSemaphoreCreateMutex();
    if (mutex_ == nullptr) return false;

    i2s_chan_config_t channel_config = {};
    channel_config.id = KORVO_AUDIO_I2S_PORT;
    channel_config.role = I2S_ROLE_MASTER;
    // Match a Host frame exactly. Partial 240-frame DMA blocks can expire
    // before the next 320-sample write. Three blocks bound hardware lookahead
    // to 60 ms, rather than enlarging the former 90-ms ring to 120 ms.
    channel_config.dma_desc_num = 3;
    channel_config.dma_frame_num = KORVO_AUDIO_SAMPLE_RATE / 50;
    channel_config.auto_clear = true;
    // RX is enabled only while a permitted Audio capture resource is open.
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
    const i2s_event_callbacks_t callbacks = {.on_sent = OnDmaSent};
    if (i2s_channel_init_std_mode(tx_channel_, &std_config) != ESP_OK ||
        i2s_channel_init_std_mode(rx_channel_, &std_config) != ESP_OK ||
        i2s_channel_register_event_callback(tx_channel_, &callbacks, this) != ESP_OK ||
        i2s_channel_enable(tx_channel_) != ESP_OK) {
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
    // Construct both control objects before opening the DAC: codec creation
    // resets common registers. The ADC must never own the speaker PA pin.
    codec_config.codec_mode = ESP_CODEC_DEV_WORK_MODE_ADC;
    codec_config.pa_pin = GPIO_NUM_NC;
    const auto* input_codec = es8389_codec_new(&codec_config);
    esp_codec_dev_cfg_t input_device = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = input_codec, .data_if = data_if_,
    };
    if (input_codec) microphone_ = esp_codec_dev_new(&input_device);
    esp_codec_dev_sample_info_t sample_info = {
        .bits_per_sample = 16,
        .channel = KORVO_AUDIO_CHANNELS,
        .channel_mask = 0,
        .sample_rate = KORVO_AUDIO_SAMPLE_RATE,
        .mclk_multiple = 0,
    };
    if (esp_codec_dev_open(speaker_, &sample_info) != ESP_OK) return false;
    SetVolume(volume());
    if (!output_.Initialize([](void* context, const int16_t* pcm, size_t samples) {
            return static_cast<Esp32S31Korvo1Audio*>(context)->Write(pcm, samples);
        }, this, true)) return false;
    ESP_LOGI(kTag, "ES8389 speaker ready at %d Hz stereo", KORVO_AUDIO_SAMPLE_RATE);
    if (!microphone_ || !capture_.Initialize(
        [](void* context, bool enabled) {
            return static_cast<Esp32S31Korvo1Audio*>(context)->SetCaptureEnabled(enabled);
        }, [](void* context, int16_t* pcm, size_t samples) {
            return static_cast<Esp32S31Korvo1Audio*>(context)->ReadCapture(pcm, samples);
        }, this)) ESP_LOGW(kTag, "Continuing without microphone capture");
    return true;
}

bool Esp32S31Korvo1Audio::SetCaptureEnabled(bool enabled) {
    if (!microphone_ || !mutex_) return false;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    int result;
    if (!enabled) result = esp_codec_dev_close(microphone_);
    else {
        esp_codec_dev_sample_info_t info = {};
        info.bits_per_sample = 16;
        info.channel = 2;
        info.sample_rate = KORVO_AUDIO_SAMPLE_RATE;
        result = esp_codec_dev_open(microphone_, &info);
        if (result == ESP_OK) result = esp_codec_dev_set_in_gain(microphone_, 24.0f);
        if (result != ESP_OK) (void)esp_codec_dev_close(microphone_);
    }
    xSemaphoreGive(mutex_);
    return result == ESP_OK;
}

int32_t Esp32S31Korvo1Audio::ReadCapture(int16_t* pcm, size_t samples) {
    if (!pcm || !samples || samples > 320) return PXA_STATUS_INVALID_ARGUMENT;
    size_t bytes = 0;
    const auto error = i2s_channel_read(rx_channel_, capture_stereo_, samples * 4, &bytes, 40);
    if (error != ESP_OK && error != ESP_ERR_TIMEOUT) return PXA_STATUS_IO_ERROR;
    if (bytes > samples * 4 || bytes % 4) return PXA_STATUS_IO_ERROR;
    for (size_t i = 0; i < bytes / 4; ++i) pcm[i] = capture_stereo_[i * 2];
    return bytes ? int32_t(bytes / 4) : PXA_STATUS_WOULD_BLOCK;
}

void Esp32S31Korvo1Audio::SetVolume(uint8_t percent) {
    if (percent > 100) percent = 100;
    volume_.store(percent);
    if (speaker_ == nullptr || mutex_ == nullptr) return;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    (void)esp_codec_dev_set_out_vol(speaker_, percent);
    xSemaphoreGive(mutex_);
}

bool Esp32S31Korvo1Audio::Write(const int16_t* pcm, size_t samples) {
    if (speaker_ == nullptr || mutex_ == nullptr || pcm == nullptr ||
        samples == 0)
        return false;
    if (samples > kMaxWriteSamples) return false;
    /* The Host streams mono PCM at the session rate while the ES8389 output and
     * the I2S bus are opened as interleaved stereo. Writing the mono samples
     * straight through makes the codec consume two samples per frame, which
     * plays the stream at double rate with a starved DMA (fast, chopped audio).
     * Duplicate each sample into the left and right slots instead. */
    bool nonzero = false;
    for (size_t index = 0; index < samples; ++index) {
        stereo_[index * 2] = pcm[index];
        stereo_[index * 2 + 1] = pcm[index];
        nonzero |= pcm[index] != 0;
    }
    if (nonzero && first_pcm_us_.load(std::memory_order_relaxed) == 0)
        first_pcm_us_.store(static_cast<uint32_t>(esp_timer_get_time()),
                            std::memory_order_release);
    xSemaphoreTake(mutex_, portMAX_DELAY);
    // The codec wrapper discards bytes_written. Check the configured I2S
    // channel directly so a short write cannot silently count as successful.
    const size_t bytes = samples * 2u * sizeof(*pcm);
    size_t written = 0;
    const esp_err_t result = i2s_channel_write(tx_channel_, stereo_, bytes, &written, 100);
    xSemaphoreGive(mutex_);
    const bool complete = result == ESP_OK && written == bytes;
    ++writes_;
    if (!complete) {
        ++write_errors_;
        ESP_LOGW(kTag, "I2S write: %s bytes=%u/%u", esp_err_to_name(result),
                 static_cast<unsigned>(written), static_cast<unsigned>(bytes));
    }
    if (writes_ % 100 == 0) {
        const uint32_t first_dma = first_dma_us_.load(std::memory_order_acquire);
        const uint32_t first_pcm = first_pcm_us_.load(std::memory_order_acquire);
        ESP_LOGI(kTag, "DMA writes=%lu blocks=%lu nonzero=%lu errors=%lu first_pcm_to_dma_us=%lu",
                 static_cast<unsigned long>(writes_),
                 static_cast<unsigned long>(dma_blocks_.load(std::memory_order_relaxed)),
                 static_cast<unsigned long>(dma_nonzero_blocks_.load(std::memory_order_relaxed)),
                 static_cast<unsigned long>(write_errors_),
                 static_cast<unsigned long>(first_dma && first_pcm ? first_dma - first_pcm : 0));
    }
    return complete;
}

bool IRAM_ATTR Esp32S31Korvo1Audio::OnDmaSent(
    i2s_chan_handle_t, i2s_event_data_t* event, void* context) {
    static_assert(std::atomic<uint32_t>::is_always_lock_free);
    auto* self = static_cast<Esp32S31Korvo1Audio*>(context);
    self->dma_blocks_.fetch_add(1, std::memory_order_relaxed);
    // IDF calls this before auto-clear. Inspect consumed PCM, not queued PCM.
    const auto* samples = static_cast<const int16_t*>(event->dma_buf);
    for (size_t i = 0; i < event->size / sizeof(int16_t); ++i) {
        if (samples[i] == 0) continue;
        self->dma_nonzero_blocks_.fetch_add(1, std::memory_order_relaxed);
        if (self->first_dma_us_.load(std::memory_order_relaxed) == 0)
            self->first_dma_us_.store(static_cast<uint32_t>(esp_timer_get_time()),
                                      std::memory_order_release);
        break;
    }
    return false;
}
