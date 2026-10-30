#pragma once
#include <audio_platform.h>
constexpr int PXA_STATUS_UNAVAILABLE=-7, PXA_STATUS_INVALID_ARGUMENT=-1, PXA_STATUS_IO_ERROR=-8, PXA_STATUS_WOULD_BLOCK=-5;

using esp_err_t = int;
using gpio_num_t = int;
using i2c_master_bus_handle_t = void*;
using i2s_chan_handle_t = void*;
using esp_codec_dev_handle_t = void*;
enum { ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_NO_MEM = -2, ESP_ERR_INVALID_STATE = -3,
       I2S_NUM_0, I2C_NUM_0, I2S_ROLE_MASTER, I2S_DATA_BIT_WIDTH_16BIT,
       I2S_SLOT_MODE_STEREO, GPIO_MODE_INPUT_OUTPUT, ESP_CODEC_DEV_WORK_MODE_DAC,
       ESP_CODEC_DEV_TYPE_OUT, ESP_CODEC_DEV_TYPE_IN, ESP_CODEC_DEV_WORK_MODE_ADC, ESP_ERR_TIMEOUT, GPIO_NUM_NC = -1 };
struct gpio_config_t { uint64_t pin_bit_mask; int mode; };
struct i2s_chan_config_t { int id, role; size_t dma_desc_num, dma_frame_num; bool auto_clear; };
struct i2s_std_config_t {
    struct { unsigned rate; } clk_cfg;
    struct { int bits, mode; } slot_cfg;
    struct { gpio_num_t mclk, bclk, ws, dout, din; } gpio_cfg;
};
#define I2S_STD_CLK_DEFAULT_CONFIG(rate) {rate}
#define I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bits, mode) {bits, mode}
struct i2s_event_data_t { void* dma_buf; size_t size; };
struct i2s_event_callbacks_t { bool (*on_sent)(i2s_chan_handle_t, i2s_event_data_t*, void*); };
struct audio_codec_data_if_t {};
struct audio_codec_gpio_if_t {};
struct audio_codec_ctrl_if_t {};
struct audio_codec_if_t {};
struct audio_codec_i2s_cfg_t { int port; void *rx_handle, *tx_handle; };
struct audio_codec_i2c_cfg_t { int port; uint8_t addr; void* bus_handle; };
struct es8311_codec_cfg_t {
    const audio_codec_ctrl_if_t* ctrl_if;
    const audio_codec_gpio_if_t* gpio_if;
    int codec_mode, pa_pin;
    bool pa_reverted, master_mode, use_mclk, digital_mic, invert_mclk, invert_sclk;
    struct { float pa_voltage, codec_dac_voltage; } hw_gain;
    bool no_dac_ref;
    unsigned mclk_div;
};
struct esp_codec_dev_cfg_t {
    int dev_type;
    const audio_codec_if_t* codec_if;
    const audio_codec_data_if_t* data_if;
};
struct esp_codec_dev_sample_info_t { int bits_per_sample, channel, channel_mask; unsigned sample_rate, mclk_multiple; };
int gpio_set_level(gpio_num_t, int);
int gpio_get_level(gpio_num_t);
int gpio_config(const gpio_config_t*);
int i2s_new_channel(const i2s_chan_config_t*, i2s_chan_handle_t*, i2s_chan_handle_t*);
int i2s_channel_init_std_mode(i2s_chan_handle_t, const i2s_std_config_t*);
int i2s_channel_register_event_callback(i2s_chan_handle_t, const i2s_event_callbacks_t*, void*);
int i2s_channel_enable(i2s_chan_handle_t);
int i2s_channel_disable(i2s_chan_handle_t);
int i2s_channel_preload_data(i2s_chan_handle_t, const void*, size_t, size_t*);
int i2s_channel_write(i2s_chan_handle_t, const void*, size_t, size_t*, unsigned);
const audio_codec_data_if_t* audio_codec_new_i2s_data(const audio_codec_i2s_cfg_t*);
const audio_codec_gpio_if_t* audio_codec_new_gpio();
const audio_codec_ctrl_if_t* audio_codec_new_i2c_ctrl(const audio_codec_i2c_cfg_t*);
const audio_codec_if_t* es8311_codec_new(const es8311_codec_cfg_t*);
esp_codec_dev_handle_t esp_codec_dev_new(const esp_codec_dev_cfg_t*);
int esp_codec_dev_open(esp_codec_dev_handle_t, const esp_codec_dev_sample_info_t*);
int esp_codec_dev_set_out_mute(esp_codec_dev_handle_t, bool);
int esp_codec_dev_set_out_vol(esp_codec_dev_handle_t, int);
int esp_codec_dev_read_reg(esp_codec_dev_handle_t, int, int*);
int esp_codec_dev_write(esp_codec_dev_handle_t, void*, size_t);
const char* esp_err_to_name(int);

class PxaAudioOutput {
public:
    struct MusicStats {
        struct { uint64_t consumed_samples, underruns, recoveries, missing_samples; } buffer;
        uint64_t decode_max_us, read_max_us;
    };
    bool Initialize(bool (*write)(void*, const int16_t*, size_t), void* context, bool paced) {
        assert(paced);
        write_ = write; context_ = context;
        active = this;
        return true;
    }
    bool GetMusicStats(MusicStats* stats) { *stats = {}; return true; }
    bool Emit(const int16_t* pcm, size_t count) { return write_(context_, pcm, count); }
    inline static PxaAudioOutput* active = nullptr;
private:
    bool (*write_)(void*, const int16_t*, size_t) = nullptr;
    void* context_ = nullptr;
};

int esp_codec_dev_close(esp_codec_dev_handle_t);
int esp_codec_dev_set_in_gain(esp_codec_dev_handle_t, float);
int i2s_channel_read(i2s_chan_handle_t, void*, size_t, size_t*, unsigned);
class PxaAudioCaptureInput {
public:
    using Control = bool(*)(void*, bool);
    using Read = int32_t(*)(void*, int16_t*, size_t);
    bool Initialize(Control control, Read read, void* context) {
        control_ = control; read_ = read; context_ = context; active = this; return true;
    }
    bool Enable(bool enabled) { opened_ = enabled; return control_(context_, enabled); }
    int32_t ReadPcm(int16_t* output, size_t samples) { return read_(context_, output, samples); }
    void StopForShutdown() { if (opened_) { Enable(false); opened_ = false; } }
    inline static PxaAudioCaptureInput* active = nullptr;
private:
    Control control_ = nullptr; Read read_ = nullptr; void* context_ = nullptr; bool opened_ = false;
};
