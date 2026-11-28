// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>

#include <esp_err.h>
#include <esp_wifi_types.h>

namespace board_io {
esp_err_t init_usb();
void report(const char* format, ...) __attribute__((format(printf, 1, 2)));
void read_wifi(wifi_config_t& config);
esp_err_t init_board();
esp_err_t init_audio(unsigned rate);
esp_err_t write_pcm(const uint8_t* pcm, size_t bytes);
void stop_audio();
esp_err_t draw_rgb565(const uint16_t* pixels, unsigned width, unsigned height);
}
