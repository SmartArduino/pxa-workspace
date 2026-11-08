// SPDX-License-Identifier: Apache-2.0
#include "esp_rom_gpio.h"
#include "hal/gpio_ll.h"
#include "soc/gpio_sig_map.h"

// ESP-IDF's bootloader linker explicitly retains this hook object.
void bootloader_hooks_include(void) {}

void bootloader_before_init(void) {
    // No heap, scheduler or initialized BSS is available here. Keep the
    // active-high backlight dark before flash verification and app startup.
    gpio_ll_set_level(&GPIO, 24, 0);
    gpio_ll_func_sel(&GPIO, 24, PIN_FUNC_GPIO);
    esp_rom_gpio_connect_out_signal(24, SIG_GPIO_OUT_IDX, false, false);
    gpio_ll_pullup_dis(&GPIO, 24);
    gpio_ll_pulldown_en(&GPIO, 24);
    gpio_ll_output_enable(&GPIO, 24);
}
