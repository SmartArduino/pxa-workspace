#include "doit_gamekit_1_panel.h"
#include "doit_gamekit_1_config.h"
#include <driver/gpio.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sdkconfig.h>

namespace {
// game_-console/components/lcd/st7789.c, CL32CK223-18A ST7789V TN sequence.
struct Command { uint8_t command, count, bytes[14]; };
constexpr Command kInitialization[] = {
    {0x3a, 1, {0x05}},
    {0xb2, 5, {0x0c, 0x0c, 0x00, 0x33, 0x33}},
    {0xb7, 1, {0x35}}, {0xbb, 1, {0x28}}, {0xc0, 1, {0x2c}},
    {0xc2, 1, {0x01}}, {0xc3, 1, {0x0b}}, {0xc4, 1, {0x20}},
    {0xc6, 1, {0x0f}}, {0xd0, 2, {0xa4, 0xa1}},
    {0xe0, 14, {0xd0, 0x01, 0x08, 0x0f, 0x11, 0x2a, 0x36,
                0x55, 0x44, 0x3a, 0x0b, 0x06, 0x11, 0x20}},
    {0xe1, 14, {0xd0, 0x02, 0x07, 0x0a, 0x0b, 0x18, 0x34,
                0x43, 0x4a, 0x2b, 0x1b, 0x1c, 0x22, 0x1f}},
};
}

bool DoitGamekit1Panel::Initialize(esp_lcd_panel_io_handle_t io) {
    using namespace doit_gamekit_1;
    io_ = io;
    if (!io_) return false;
    const gpio_config_t reset = {
        .pin_bit_mask = 1ULL << kLcdReset, .mode = GPIO_MODE_OUTPUT,
    };
    if (gpio_config(&reset) != ESP_OK) return false;
    gpio_set_level(gpio_num_t(kLcdReset), 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(gpio_num_t(kLcdReset), 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(gpio_num_t(kLcdReset), 1);
    vTaskDelay(pdMS_TO_TICKS(120));
    if (esp_lcd_panel_io_tx_param(io_, 0x01, nullptr, 0) != ESP_OK) return false;
    vTaskDelay(pdMS_TO_TICKS(150));
    if (esp_lcd_panel_io_tx_param(io_, 0x11, nullptr, 0) != ESP_OK) return false;
    vTaskDelay(pdMS_TO_TICKS(120));
#if CONFIG_PXA_DOIT1_LCD_FLIP_180
    uint8_t madctl = 0xa0;
#else
    uint8_t madctl = 0x60;
#endif
#if CONFIG_PXA_DOIT1_LCD_BGR
    madctl |= 0x08;
#endif
    for (const auto& command : kInitialization) {
        if (esp_lcd_panel_io_tx_param(io_, command.command, command.bytes, command.count) != ESP_OK)
            return false;
        // Reference ordering: COLMOD=0x05 followed by MADCTL=0x60.
        if (command.command == 0x3a && esp_lcd_panel_io_tx_param(io_, 0x36, &madctl, 1) != ESP_OK)
            return false;
    }
#if CONFIG_PXA_DOIT1_LCD_INVERT
    constexpr int inversion = 0x21;
#else
    constexpr int inversion = 0x20;
#endif
    if (esp_lcd_panel_io_tx_param(io_, inversion, nullptr, 0) != ESP_OK ||
        esp_lcd_panel_io_tx_param(io_, 0x13, nullptr, 0) != ESP_OK) return false;
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_LOGI("gamekit1_panel", "ST7789V TN reference init: COLMOD=0x05 MADCTL=0x%02x, inversion=%s",
             madctl, inversion == 0x20 ? "off" : "on");
    return true;
}

esp_err_t DoitGamekit1Panel::DisplayOn() {
    const auto error = esp_lcd_panel_io_tx_param(io_, 0x29, nullptr, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    return error;
}

esp_err_t DoitGamekit1Panel::Draw(const uint16_t* native) {
    using namespace doit_gamekit_1;
    constexpr int x = CONFIG_PXA_DOIT1_LCD_X_GAP, y = CONFIG_PXA_DOIT1_LCD_Y_GAP;
    constexpr int right = x + kNativeWidth - 1, bottom = y + kNativeHeight - 1;
    const uint8_t columns[] = {uint8_t(x >> 8), uint8_t(x), uint8_t(right >> 8), uint8_t(right)};
    const uint8_t rows[] = {uint8_t(y >> 8), uint8_t(y), uint8_t(bottom >> 8), uint8_t(bottom)};
    auto error = esp_lcd_panel_io_tx_param(io_, 0x2a, columns, sizeof(columns));
    if (error == ESP_OK) error = esp_lcd_panel_io_tx_param(io_, 0x2b, rows, sizeof(rows));
    return error == ESP_OK ? esp_lcd_panel_io_tx_color(io_, 0x2c, native,
                                     kNativeWidth * kNativeHeight * sizeof(uint16_t)) : error;
}
