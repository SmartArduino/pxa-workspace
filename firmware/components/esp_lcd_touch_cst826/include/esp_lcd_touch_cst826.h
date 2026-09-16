/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file esp_lcd_touch_cst826.h
 * @brief ESP LCD Touch CST826 Controller
 */

#pragma once

#include "esp_err.h"
#include "esp_lcd_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief CST826 I2C address
 */
#define ESP_LCD_TOUCH_IO_I2C_CST826_ADDRESS    (0x15)

/**
 * @brief Create a new CST826 touch driver
 *
 * @note The I2C communication should be initialized before use this function.
 *
 * @param io LCD panel IO handle, it should be created by `esp_lcd_new_panel_io_i2c()`
 * @param config Touch panel configuration
 * @param tp Touch panel handle
 * @return
 *      - ESP_OK: on success
 *      - ESP_ERR_NO_MEM: if there is no memory for allocating main structure
 */
esp_err_t esp_lcd_touch_new_i2c_cst826(const esp_lcd_panel_io_handle_t io, const esp_lcd_touch_config_t *config, esp_lcd_touch_handle_t *tp);

/**
 * @brief CST826 configuration structure
 */
#define ESP_LCD_TOUCH_IO_I2C_CST826_CONFIG()                   \
    {                                                          \
        .dev_addr = ESP_LCD_TOUCH_IO_I2C_CST826_ADDRESS,       \
        .control_phase_bytes = 1,                              \
        .dc_bit_offset = 0,                                    \
        .lcd_cmd_bits = 8,                                     \
        .flags =                                               \
        {                                                      \
            .disable_control_phase = 1,                        \
        }                                                      \
    }

#ifdef __cplusplus
}
#endif 