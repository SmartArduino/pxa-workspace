// SPDX-FileCopyrightText: 2026 SmartArduino
// SPDX-License-Identifier: Apache-2.0
// ST7102 vendor command table from SmartArduino/ESPP4-A, commit
// 1e981d9f159a1afb30bd5e41b0137344ce3916a6. Adapted for the PXA board port.
#include "doit_s31_panel.h"
#include <driver/gpio.h>
#include <esp_lcd_panel_rgb.h>
#include <esp_rom_sys.h>


#include <esp_check.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
typedef struct {
    uint8_t cmd;
    const uint8_t *data;
    uint8_t len;
    uint16_t delay_ms;
} lcd_init_cmd_t;

static const lcd_init_cmd_t init_cmds[] = {
    {0x10, NULL, 0, 5},
    {0x28, NULL, 0, 5},
    {0x99, (uint8_t[]){0x71, 0x02, 0xa2}, 3, 0},
    {0x99, (uint8_t[]){0x71, 0x02, 0xa3}, 3, 0},
    {0x99, (uint8_t[]){0x71, 0x02, 0xa4}, 3, 0},
    {0xA4, (uint8_t[]){0x31}, 1, 0},
    {0xB0, (uint8_t[]){0x22, 0x6B, 0x1E, 0x75, 0x2F, 0x39, 0x43}, 7, 0},
    {0xB7, (uint8_t[]){0x7D, 0x7D}, 2, 0},
    {0xBF, (uint8_t[]){0x90, 0x90}, 2, 0},
    {0xBA, (uint8_t[]){0x72, 0x76, 0x52, 0xD6, 0xC0, 0x20, 0x00, 0x00}, 8, 0},
    {0xD7, (uint8_t[]){0x00, 0x0B, 0xC6, 0x19, 0xAB, 0xAB}, 6, 0},
    {0xA3, (uint8_t[]){0x51, 0x03, 0x88, 0x00, 0x44, 0x00, 0x00, 0x00, 0x00, 0x04, 0x5C, 0xCF, 0x00, 0x1A, 0x00, 0x45,
                       0x05, 0x00, 0x00, 0x00, 0x00, 0x46, 0x00, 0x00, 0x02, 0x20, 0x52, 0x00, 0x00, 0x00, 0x00, 0xFF}, 32, 0},
    {0xA7, (uint8_t[]){0x19, 0x19, 0x00, 0x64, 0x40, 0x05, 0x14, 0x40, 0x00, 0x04, 0x03, 0xCF, 0xCF, 0x00, 0x64, 0x40,
                       0x23, 0x67, 0x01, 0x00, 0x02, 0x00, 0xCF, 0xCF, 0x00, 0x64, 0x40, 0x4B, 0x5A, 0x00, 0x00, 0x02,
                       0x01, 0xCF, 0xCF, 0x00, 0x24, 0x40, 0x69, 0x78, 0x00, 0x00, 0x00, 0x00, 0xCF, 0xCF, 0x00, 0x44}, 48, 0},
    {0xA6, (uint8_t[]){0x30, 0x00, 0x24, 0x77, 0x36, 0x00, 0x37, 0x00, 0x5C, 0xCF, 0x02, 0xE4, 0x99, 0x00, 0x0B, 0x00,
                       0x0B, 0xCF, 0x5C, 0x02, 0xA4, 0x11, 0x00, 0x00, 0x00, 0x00, 0x5C, 0xCF, 0x00, 0xAC, 0x11, 0x00,
                       0x00, 0x00, 0x00, 0x5C, 0xCF, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00}, 44, 0},
    {0xA7, (uint8_t[]){0x19, 0x19, 0x00, 0x64, 0x40, 0x05, 0x14, 0x40, 0x00, 0x04, 0x03, 0x5C, 0xCF, 0x00, 0x64, 0x40,
                       0x23, 0x67, 0x01, 0x00, 0x02, 0x00, 0x5C, 0xCF, 0x00, 0x64, 0x40, 0x4B, 0x5A, 0x00, 0x00, 0x02,
                       0x01, 0x5C, 0xCF, 0x00, 0x24, 0x40, 0x69, 0x78, 0x00, 0x00, 0x00, 0x00, 0x5C, 0xCF, 0x00, 0x44}, 48, 0},
    {0xAC, (uint8_t[]){0x13, 0x10, 0x01, 0x00, 0x1C, 0x12, 0x02, 0x1B, 0x18, 0x09, 0x19, 0x1A, 0x1B, 0x1B, 0x18, 0x1B,
                       0x0B, 0x11, 0x00, 0x02, 0x1C, 0x0A, 0x02, 0x18, 0x18, 0x08, 0x19, 0x1A, 0x1B, 0x1B, 0x18, 0x1B,
                       0xFF, 0xFF, 0xFF, 0xFF, 0x00}, 37, 0},
    {0xAD, (uint8_t[]){0xCC, 0x40, 0x46, 0x11, 0x04, 0x5C, 0xCF}, 7, 0},
    {0xE8, (uint8_t[]){0x30, 0x07, 0x00, 0xEB, 0xEB, 0x9C, 0x00, 0xE2, 0x04, 0x00, 0x00, 0x00, 0x00, 0xEF}, 14, 0},
    {0xE9, (uint8_t[]){0x3C, 0x7F, 0x08, 0x0C, 0x1A, 0x7A, 0x22, 0x1A, 0x33}, 9, 0},
    {0x75, (uint8_t[]){0x03, 0x04}, 2, 0},
    {0xE7, (uint8_t[]){0x8B, 0x3C, 0x00, 0x0C, 0xF0, 0x5D, 0x00, 0x5D, 0x05, 0x5D, 0x00, 0x5D, 0x00, 0xFF, 0x00, 0x08,
                       0x7B, 0x00, 0x00, 0xC8, 0x6A, 0x5A, 0x08, 0x1A, 0x3C, 0x00, 0x91, 0x01, 0xCC, 0x01, 0x7F, 0xF0,
                       0x22}, 33, 0},
    {0xC8, (uint8_t[]){0x00, 0x00, 0x15, 0x26, 0x44, 0x00, 0x78, 0x03, 0xBE, 0x06, 0x11, 0x1C, 0x09, 0x8A, 0x03, 0x21,
                       0xD4, 0x01, 0x11, 0x0F, 0x22, 0x4A, 0x0F, 0x8F, 0x0A, 0x32, 0xF0, 0x0A, 0x41, 0x0D, 0xF3, 0x80,
                       0x0D, 0xAE, 0xC5, 0x03, 0xC4}, 37, 0},
    {0xC9, (uint8_t[]){0x00, 0x00, 0x15, 0x26, 0x44, 0x00, 0x78, 0x03, 0xBE, 0x06, 0x11, 0x1C, 0x09, 0x8A, 0x03, 0x21,
                       0xD4, 0x01, 0x11, 0x0F, 0x22, 0x4A, 0x0F, 0x8F, 0x0A, 0x32, 0xF0, 0x0A, 0x41, 0x0D, 0xF3, 0x80,
                       0x0D, 0xAE, 0xC5, 0x03, 0xC4}, 37, 0},
    {0x3a, (uint8_t[]){0x77}, 1, 0}, // RGB uses the high bits of each 8-bit channel.
    {0x11, NULL, 0, 150},
    {0x29, NULL, 0, 50},
};


// The panel control SPI shares SDA/SCL with codec/touch I2C. Run it only
// before creating I2C0, using open-drain pins to avoid driving against ACKs.
// GPIO60 is panel SDO (schematic net LCD_MOSI); it must remain an input.
static void spi_word(unsigned word) {
    for (int bit = 8; bit >= 0; --bit) {
        gpio_set_level(1, 0);
        gpio_set_level(0, (word >> bit) & 1);
        esp_rom_delay_us(3);
        gpio_set_level(1, 1);
        esp_rom_delay_us(3);
    }
}
static void command(uint8_t cmd, const uint8_t *data, unsigned len) {
    gpio_set_level(61, 0);
    spi_word(cmd);
    for (unsigned i = 0; i < len; ++i) spi_word(0x100 | data[i]);
    gpio_set_level(61, 1);
}
static uint8_t read_register(uint8_t cmd) {
    uint8_t result = 0;
    gpio_set_level(61, 0); spi_word(cmd); gpio_set_level(0, 1);
    // The controller presents its first bit with the command's last edge.
    // Sample that bit before advancing SCL; extra turnaround clocks lose MSBs.
    for (unsigned i = 0; i < 8; ++i) {
        result = (result << 1) | gpio_get_level(60);
        gpio_set_level(1, 0); esp_rom_delay_us(3);
        gpio_set_level(1, 1); esp_rom_delay_us(3);
    }
    gpio_set_level(61, 1);
    return result;
}
esp_err_t doit_s31_panel_create(esp_lcd_panel_handle_t *panel) {
    const gpio_config_t control = {.pin_bit_mask = 1ULL << 9,
        .mode = GPIO_MODE_OUTPUT};
    ESP_RETURN_ON_ERROR(gpio_config(&control), "doit_panel", "control pins");
    const gpio_config_t cs = {.pin_bit_mask = 1ULL << 61,
        .mode = GPIO_MODE_OUTPUT_OD, .pull_up_en = GPIO_PULLUP_ENABLE};
    ESP_RETURN_ON_ERROR(gpio_config(&cs), "doit_panel", "CS/BOOT pin");
    gpio_set_level(61, 1);
    const gpio_config_t serial = {.pin_bit_mask = 3, .mode = GPIO_MODE_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE};
    ESP_RETURN_ON_ERROR(gpio_config(&serial), "doit_panel", "shared serial pins");
    gpio_set_level(0, 1); gpio_set_level(1, 1);
    // One common reset for display and touch; never reset touch a second time.
    gpio_set_level(9, 1); vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(9, 0); vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(9, 1); vTaskDelay(pdMS_TO_TICKS(120));
    for (unsigned i = 0; i < sizeof(init_cmds) / sizeof(init_cmds[0]); ++i) {
        command(init_cmds[i].cmd, init_cmds[i].data, init_cmds[i].len);
        if (init_cmds[i].delay_ms) vTaskDelay(pdMS_TO_TICKS(init_cmds[i].delay_ms));
    }
    const gpio_config_t sdo = {.pin_bit_mask = 1ULL << 60, .mode = GPIO_MODE_INPUT};
    ESP_RETURN_ON_ERROR(gpio_config(&sdo), "doit_panel", "SDO pin");
    const uint8_t power = read_register(0x0a), color = read_register(0x0c);
    ESP_LOGI("doit_panel", "ST7102 SPI9 power=0x%02x color=0x%02x", power, color);
    if (power == 0 || power == 0xff)
        ESP_LOGW("doit_panel", "Panel readback unavailable; verify panel interface straps and SPI wiring");
    // Recover an incidental I2C start during SPI and finish with STOP.
    gpio_set_level(0, 1);
    for (int i = 0; i < 9; ++i) {
        gpio_set_level(1, 0); esp_rom_delay_us(5);
        gpio_set_level(1, 1); esp_rom_delay_us(5);
    }
    gpio_set_level(1, 0); gpio_set_level(0, 0); esp_rom_delay_us(5);
    gpio_set_level(1, 1); esp_rom_delay_us(5); gpio_set_level(0, 1);
    gpio_reset_pin(0); gpio_reset_pin(1);
    // Vendor timing image: 565 clocks/line, 1061 lines/frame, about 60 Hz.
    esp_lcd_rgb_panel_config_t rgb = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {.pclk_hz = 36000000, .h_res = 480, .v_res = 854,
            .hsync_pulse_width = 5, .hsync_back_porch = 40, .hsync_front_porch = 40,
            .vsync_pulse_width = 5, .vsync_back_porch = 9, .vsync_front_porch = 193,
            .flags = {.pclk_active_neg = true}},
        .data_width = 24, .in_color_format = LCD_COLOR_FMT_RGB565,
        .out_color_format = LCD_COLOR_FMT_RGB888,
        .num_fbs = 2, .dma_burst_size = 64,
        .hsync_gpio_num = 42, .vsync_gpio_num = 40, .de_gpio_num = 44,
        .pclk_gpio_num = 43, .disp_gpio_num = -1,
        // The panel COLMOD is RGB888. Preserve 24 peripheral slots even
        // though only B3..7/G2..7/R3..7 are physically connected. The LCD
        // converter expands the RGB565 framebuffer before driving these slots.
        .data_gpio_nums = {
            -1, -1, -1, 57, 7, 5, 6, 8,
            -1, -1, 51, 52, 53, 54, 55, 56,
            -1, -1, -1, 36, 35, 34, 33, 50,
        },
        .flags = {.fb_in_psram = true},
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_rgb_panel(&rgb, panel), "doit_panel", "RGB panel");
    return esp_lcd_panel_reset(*panel);
}
