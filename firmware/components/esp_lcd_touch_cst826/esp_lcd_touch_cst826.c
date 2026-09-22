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
/* Hynitron CST7xx protocol: every contact occupies six bytes
 * (XH, XL, YH, YL, pressure_high, area) right after the touch-num register. */
#define POINT_RECORD_BYTES  (6)
#define POINT_REPORT_BYTES  (1 + POINT_RECORD_BYTES * POINT_NUM_MAX)

#define DATA_START_REG      (0x02)
#define CHIP_ID_REG         (0xA7)
#define CST826_CHIP_ID      (0xCA)

static const char *TAG = "CST826";

/* The public handle stays first so it doubles as the driver-private storage
 * for the finger ids delivered by the controller. */
typedef struct {
    esp_lcd_touch_t base;
    uint8_t track_ids[POINT_NUM_MAX];
    uint8_t last_reported;
} cst826_t;

static esp_err_t read_data(esp_lcd_touch_handle_t tp);
static bool get_xy(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y, uint16_t *strength, uint8_t *point_num, uint8_t max_point_num);
static esp_err_t get_track_id(esp_lcd_touch_handle_t tp, uint8_t *track_id, uint8_t point_num);
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
    esp_lcd_touch_handle_t cst826 = NULL;
    cst826_t *handle = calloc(1, sizeof(cst826_t));
    ESP_GOTO_ON_FALSE(handle, ESP_ERR_NO_MEM, err, TAG, "Touch handle malloc failed");
    cst826 = &handle->base;

    /* Communication interface */
    cst826->io = io;
    /* Only supported callbacks are set */
    cst826->read_data = read_data;
    cst826->get_xy = get_xy;
    cst826->get_track_id = get_track_id;
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
    cst826_t *handle = (cst826_t *)tp;
    uint8_t report[POINT_REPORT_BYTES] = {0};
    ESP_RETURN_ON_ERROR(i2c_read_bytes(tp, DATA_START_REG, report, sizeof(report)), TAG, "I2C read failed");

    uint8_t reported = report[0] & 0x0F;
    if (reported > POINT_NUM_MAX) reported = POINT_NUM_MAX;

    portENTER_CRITICAL(&tp->data.lock);
    uint8_t valid = 0;
    for (uint8_t i = 0; i < reported; i++) {
        const uint8_t *record = &report[1 + i * POINT_RECORD_BYTES];
        /* event_flg 0b11 marks an unused/stale record on this family. */
        if ((record[0] & 0xC0) == 0xC0) continue;
        tp->data.coords[valid].x = (record[0] & 0x0F) << 8 | record[1];
        tp->data.coords[valid].y = (record[2] & 0x0F) << 8 | record[3];
        tp->data.coords[valid].strength = 50;
        tp->data.coords[valid].track_id = record[2] >> 4;
        handle->track_ids[valid] = record[2] >> 4;
        valid++;
    }
    tp->data.points = valid;
    portEXIT_CRITICAL(&tp->data.lock);

    /* Log only on contact-count changes, so a second finger shows up once
     * without flooding the console at the 5 ms polling rate. */
    if (reported != handle->last_reported) {
        handle->last_reported = reported;
        if (reported > 1) {
            ESP_LOGI(TAG, "multi-touch raw num=%u [%02X %02X %02X %02X %02X %02X] [%02X %02X %02X %02X %02X %02X]",
                     reported,
                     report[1], report[2], report[3], report[4], report[5], report[6],
                     report[7], report[8], report[9], report[10], report[11], report[12]);
        }
    }

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

static esp_err_t get_track_id(esp_lcd_touch_handle_t tp, uint8_t *track_id, uint8_t point_num)
{
    ESP_RETURN_ON_FALSE(track_id, ESP_ERR_INVALID_ARG, TAG, "Invalid track_id");
    cst826_t *handle = (cst826_t *)tp;

    portENTER_CRITICAL(&tp->data.lock);
    for (uint8_t i = 0; i < point_num && i < POINT_NUM_MAX; i++) {
        track_id[i] = handle->track_ids[i];
    }
    portEXIT_CRITICAL(&tp->data.lock);

    return ESP_OK;
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
