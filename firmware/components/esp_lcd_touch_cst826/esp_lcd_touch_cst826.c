/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_system.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_cst826.h"

#define POINT_NUM_MAX       (2)

#define DATA_START_REG      (0x02)
#define CHIP_ID_REG         (0xA7)
#define CST826_CHIP_ID      (0xCA)

static const char *TAG = "CST826";

static esp_err_t read_data(esp_lcd_touch_handle_t tp);
static bool get_xy(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y, uint16_t *strength, uint8_t *point_num, uint8_t max_point_num);
static esp_err_t del(esp_lcd_touch_handle_t tp);

static esp_err_t i2c_read_bytes(esp_lcd_touch_handle_t tp, uint16_t reg, uint8_t *data, uint8_t len);

static esp_err_t reset(esp_lcd_touch_handle_t tp);
static esp_err_t read_id(esp_lcd_touch_handle_t tp);

esp_err_t esp_lcd_touch_new_i2c_cst826(const esp_lcd_panel_io_handle_t io, const esp_lcd_touch_config_t *config, esp_lcd_touch_handle_t *tp)
{
    ESP_RETURN_ON_FALSE(io, ESP_ERR_INVALID_ARG, TAG, "Invalid io");
    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "Invalid config");
    ESP_RETURN_ON_FALSE(tp, ESP_ERR_INVALID_ARG, TAG, "Invalid touch handle");

    /* Prepare main structure */
    esp_err_t ret = ESP_OK;
    esp_lcd_touch_handle_t cst826 = calloc(1, sizeof(esp_lcd_touch_t));
    ESP_GOTO_ON_FALSE(cst826, ESP_ERR_NO_MEM, err, TAG, "Touch handle malloc failed");

    /* Communication interface */
    cst826->io = io;
    /* Only supported callbacks are set */
    cst826->read_data = read_data;
    cst826->get_xy = get_xy;
    cst826->del = del;
    /* Mutex */
    cst826->data.lock.owner = portMUX_FREE_VAL;
    /* Save config */
    memcpy(&cst826->config, config, sizeof(esp_lcd_touch_config_t));

    /* Prepare pin for touch interrupt */
    if (cst826->config.int_gpio_num != GPIO_NUM_NC) {
        const gpio_config_t int_gpio_config = {
            .mode = GPIO_MODE_INPUT,
            .intr_type = (cst826->config.levels.interrupt ? GPIO_INTR_POSEDGE : GPIO_INTR_NEGEDGE),
            .pin_bit_mask = BIT64(cst826->config.int_gpio_num)
        };
        ESP_GOTO_ON_ERROR(gpio_config(&int_gpio_config), err, TAG, "GPIO intr config failed");

        /* Register interrupt callback */
        if (cst826->config.interrupt_callback) {
            esp_lcd_touch_register_interrupt_callback(cst826, cst826->config.interrupt_callback);
        }
    }
    /* Prepare pin for touch controller reset */
    if (cst826->config.rst_gpio_num != GPIO_NUM_NC) {
        const gpio_config_t rst_gpio_config = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = BIT64(cst826->config.rst_gpio_num)
        };
        ESP_GOTO_ON_ERROR(gpio_config(&rst_gpio_config), err, TAG, "GPIO reset config failed");
    }
    /* Reset controller */
    ESP_GOTO_ON_ERROR(reset(cst826), err, TAG, "Reset failed");
    /* Read product id */
    ESP_GOTO_ON_ERROR(read_id(cst826), err, TAG, "Read ID failed");
    *tp = cst826;

    return ESP_OK;
err:
    if (cst826) {
        del(cst826);
    }
    return ret;
}

static esp_err_t read_data(esp_lcd_touch_handle_t tp)
{
    typedef struct {
        uint8_t num;
        uint8_t x_h : 4;
        uint8_t : 4;
        uint8_t x_l;
        uint8_t y_h : 4;
        uint8_t : 4;
        uint8_t y_l;
        uint8_t x2_h : 4;
        uint8_t : 4;
        uint8_t x2_l;
        uint8_t y2_h : 4;
        uint8_t : 4;
        uint8_t y2_l;
    } data_t;

    data_t point;
    ESP_RETURN_ON_ERROR(i2c_read_bytes(tp, DATA_START_REG, (uint8_t *)&point, sizeof(data_t)), TAG, "I2C read failed");

    portENTER_CRITICAL(&tp->data.lock);
    point.num = (point.num > POINT_NUM_MAX ? POINT_NUM_MAX : point.num);
    tp->data.points = point.num;
    
    /* Fill all coordinates */
    if (point.num >= 1) {
        tp->data.coords[0].x = point.x_h << 8 | point.x_l;
        tp->data.coords[0].y = point.y_h << 8 | point.y_l;
        tp->data.coords[0].strength = 50;
    }
    if (point.num >= 2) {
        tp->data.coords[1].x = point.x2_h << 8 | point.x2_l;
        tp->data.coords[1].y = point.y2_h << 8 | point.y2_l;
        tp->data.coords[1].strength = 50;
    }
    
    portEXIT_CRITICAL(&tp->data.lock);

    return ESP_OK;
}

static bool get_xy(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y, uint16_t *strength, uint8_t *point_num, uint8_t max_point_num)
{
    portENTER_CRITICAL(&tp->data.lock);
    /* Count of points */
    *point_num = (tp->data.points > max_point_num ? max_point_num : tp->data.points);
    for (size_t i = 0; i < *point_num; i++) {
        x[i] = tp->data.coords[i].x;
        y[i] = tp->data.coords[i].y;

        if (strength) {
            strength[i] = tp->data.coords[i].strength;
        }
    }
    /* Invalidate */
    tp->data.points = 0;
    portEXIT_CRITICAL(&tp->data.lock);

    return (*point_num > 0);
}

static esp_err_t del(esp_lcd_touch_handle_t tp)
{
    /* Reset GPIO pin settings */
    if (tp->config.int_gpio_num != GPIO_NUM_NC) {
        gpio_reset_pin(tp->config.int_gpio_num);
        if (tp->config.interrupt_callback) {
            gpio_isr_handler_remove(tp->config.int_gpio_num);
        }
    }
    if (tp->config.rst_gpio_num != GPIO_NUM_NC) {
        gpio_reset_pin(tp->config.rst_gpio_num);
    }
    /* Release memory */
    free(tp);

    return ESP_OK;
}

static esp_err_t reset(esp_lcd_touch_handle_t tp)
{
    if (tp->config.rst_gpio_num != GPIO_NUM_NC) {
        ESP_RETURN_ON_ERROR(gpio_set_level(tp->config.rst_gpio_num, tp->config.levels.reset), TAG, "GPIO set level failed");
        vTaskDelay(pdMS_TO_TICKS(200));
        ESP_RETURN_ON_ERROR(gpio_set_level(tp->config.rst_gpio_num, !tp->config.levels.reset), TAG, "GPIO set level failed");
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    return ESP_OK;
}

static esp_err_t read_id(esp_lcd_touch_handle_t tp)
{
    uint8_t id;
    ESP_RETURN_ON_ERROR(i2c_read_bytes(tp, CHIP_ID_REG, &id, 1), TAG, "I2C read failed");
    
    // CST826的芯片ID可能不是标准的0xCA，不强制检查
    if (id != CST826_CHIP_ID) {
        ESP_LOGD(TAG, "Chip ID: 0x%02X (expected 0x%02X)", id, CST826_CHIP_ID);
    }

    return ESP_OK;
}

static esp_err_t i2c_read_bytes(esp_lcd_touch_handle_t tp, uint16_t reg, uint8_t *data, uint8_t len)
{
    ESP_RETURN_ON_FALSE(data && len, ESP_ERR_INVALID_ARG, TAG, "Invalid data or length");
    return esp_lcd_panel_io_rx_param(tp->io, reg, data, len);
}
