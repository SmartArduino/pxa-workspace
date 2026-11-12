#pragma once
#include <cstdint>
#include <esp_adc/adc_oneshot.h>
using gpio_num_t = int;
enum { GPIO_MODE_INPUT = 1 };
struct gpio_config_t {
    uint64_t pin_bit_mask = 0;
    int mode = 0, pull_up_en = 0, pull_down_en = 0;
};
esp_err_t gpio_config(const gpio_config_t* config);
int gpio_get_level(gpio_num_t gpio);
