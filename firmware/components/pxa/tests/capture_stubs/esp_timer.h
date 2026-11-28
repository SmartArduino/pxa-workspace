// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>

constexpr int ESP_OK = 0;
using esp_timer_handle_t = void*;
struct esp_timer_create_args_t {
    void (*callback)(void*);
    void* arg;
    const char* name;
};
int esp_timer_create(const esp_timer_create_args_t*, esp_timer_handle_t*);
int esp_timer_start_once(esp_timer_handle_t, uint64_t);
