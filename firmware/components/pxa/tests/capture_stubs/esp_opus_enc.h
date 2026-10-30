#pragma once
#include <audio_platform.h>
#include <mutex>
using portMUX_TYPE = std::mutex*;
inline std::mutex capture_test_lock;
#define portMUX_INITIALIZER_UNLOCKED (&capture_test_lock)
#define portENTER_CRITICAL(mux) (*(mux))->lock()
#define portEXIT_CRITICAL(mux) (*(mux))->unlock()
#define xQueueSendToBack xQueueSend
constexpr int ESP_OPUS_ENC_FRAME_DURATION_20_MS = 20;
struct esp_opus_enc_config_t { int sample_rate, channel, bits_per_sample, bitrate, frame_duration, complexity; };
#define ESP_OPUS_ENC_CONFIG_DEFAULT() {16000, 1, 16, 16000, 20, 0}
struct esp_audio_enc_in_frame_t { uint8_t* buffer; size_t len; };
struct esp_audio_enc_out_frame_t { uint8_t* buffer; size_t len, encoded_bytes; int pts; };
int esp_opus_enc_open(esp_opus_enc_config_t*, size_t, void**);
int esp_opus_enc_get_frame_size(void*, int*, int*);
int esp_opus_enc_process(void*, esp_audio_enc_in_frame_t*, esp_audio_enc_out_frame_t*);
void esp_opus_enc_close(void*);
