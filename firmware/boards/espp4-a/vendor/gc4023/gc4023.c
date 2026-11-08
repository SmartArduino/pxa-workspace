// SPDX-FileCopyrightText: 2026 SmartArduino
// SPDX-FileCopyrightText: 2001-2025 Shenshu Tech. Co., Ltd.
// SPDX-License-Identifier: Apache-2.0 AND MIT
// ESP32 adaptation: SmartArduino/ESPP4-A 1e981d9f159a1afb30bd5e41b0137344ce3916a6.
// Sensor register/gain data: OpenIPC/openhisilicon sensor libraries (MIT),
// c0d8112da51c8e9f6215a2208bdeaefc1b6201bf; see NOTICE and LICENSE.MIT.
// esp_cam_sensor driver for GC4023: MIPI 2-lane RAW10 1920x1080 25fps.
// Structured after the gc4053 driver in esp_cam_sensor.
#include <string.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_cam_sensor.h"
#include "esp_cam_sensor_detect.h"
#include "esp_sccb_io_interface.h"
#include "gc4023.h"
#include "gc4023_regs.h"

#define GC4023_VTS          1650   // frame length in lines, see gc4023_regs.h
#define GC4023_TLINE_NS     24242  // line time: 1500 lines at 27.5fps -> 1s / 27.5 / 1500
#define GC4023_EXP_MIN      4
#define GC4023_EXP_MAX      (GC4023_VTS - 8)  // longer exposure would stretch the frame
#define GC4023_EXP_DEF      1000
#define US_TO_LINES(us)     ((uint32_t)((us) * 1000 / GC4023_TLINE_NS))
#define LINES_TO_US(lines)  ((int32_t)((lines) * GC4023_TLINE_NS / 1000))
#define EXPOSURE_V4L2_UNIT_US 100  // ESP_CAM_SENSOR_EXPOSURE_US is in units of 100 us, same as other drivers

static const char *TAG = "gc4023";

// Analog gain steps, verified against MIT g_reg_val_table in OpenHisilicon gc4023_cmos.c.
// Each step writes 9 registers: 0x02cd 0x0614 0x0615 0x0218 0x1467 0x1468 0x00b8 0x00b9 0x1466
static const uint8_t gc4023_gain_regs[25][9] = {
    {0x92, 0x00, 0x00, 0x00, 0x2f, 0x2f, 0x01, 0x00, 0x1e},
    {0x92, 0x80, 0x02, 0x00, 0x30, 0x30, 0x01, 0x0B, 0x1e},
    {0x92, 0x01, 0x00, 0x00, 0x29, 0x29, 0x01, 0x19, 0x18},
    {0x92, 0x81, 0x02, 0x00, 0x2b, 0x2b, 0x01, 0x2A, 0x18},
    {0x92, 0x02, 0x00, 0x00, 0x27, 0x27, 0x02, 0x00, 0x14},
    {0x92, 0x82, 0x02, 0x00, 0x28, 0x28, 0x02, 0x17, 0x14},
    {0x92, 0x03, 0x00, 0x00, 0x29, 0x29, 0x02, 0x33, 0x14},
    {0x92, 0x83, 0x02, 0x00, 0x2b, 0x2b, 0x03, 0x14, 0x14},
    {0x92, 0x04, 0x00, 0x00, 0x2c, 0x2c, 0x04, 0x00, 0x14},
    {0x99, 0x80, 0x02, 0x20, 0x23, 0x23, 0x04, 0x2F, 0x16},
    {0x99, 0x01, 0x00, 0x20, 0x24, 0x24, 0x05, 0x26, 0x18},
    {0x99, 0x81, 0x02, 0x20, 0x27, 0x27, 0x06, 0x28, 0x1a},
    {0x99, 0x02, 0x00, 0x20, 0x29, 0x29, 0x08, 0x00, 0x1c},
    {0x99, 0x82, 0x02, 0x20, 0x2c, 0x2c, 0x09, 0x1E, 0x1f},
    {0x99, 0x03, 0x00, 0x20, 0x31, 0x31, 0x0B, 0x0C, 0x24},
    {0x99, 0x83, 0x02, 0x20, 0x35, 0x35, 0x0D, 0x11, 0x29},
    {0x99, 0x04, 0x00, 0x20, 0x3a, 0x3a, 0x10, 0x00, 0x2E},
    {0x99, 0x84, 0x02, 0x20, 0x3c, 0x3c, 0x12, 0x3D, 0x30},
    {0x99, 0x05, 0x00, 0x20, 0x3d, 0x3d, 0x16, 0x19, 0x32},
    {0x99, 0x85, 0x02, 0x20, 0x3f, 0x3f, 0x1A, 0x22, 0x34},
    {0x99, 0xb5, 0x04, 0x20, 0x3d, 0x3d, 0x20, 0x00, 0x32},
    {0x99, 0x85, 0x05, 0x20, 0x3b, 0x3b, 0x25, 0x3A, 0x30},
    {0x99, 0x05, 0x08, 0x20, 0x34, 0x34, 0x2C, 0x33, 0x29},
    {0x99, 0x45, 0x09, 0x20, 0x2b, 0x2b, 0x35, 0x05, 0x20},
    {0x99, 0x55, 0x0a, 0x20, 0x26, 0x26, 0x40, 0x00, 0x1a},
};

// actual gain of each step x 1000 (IPA picks steps from this table)
static const uint32_t gc4023_gain_x1000[25] = {
    1000, 1185, 1414, 1675, 1959, 2329, 2767, 3289, 3843, 4537, 5364, 6358, 7430,
    8827, 10485, 12435, 14513, 17419, 20817, 24693, 29349, 31627, 41111, 48713, 58851,
};

static const esp_cam_sensor_isp_info_t gc4023_isp_info = {
    .isp_v1_info = {
        .version = SENSOR_ISP_INFO_VERSION_DEFAULT,
        .pclk = 49500000,  // HTS 1200 / 24.242 us per line
        .hts = 1200,
        .vts = GC4023_VTS,
        .exp_def = GC4023_EXP_DEF,
        .gain_def = 0,
        .tline_ns = GC4023_TLINE_NS,
        .bayer_type = ESP_CAM_SENSOR_BAYER_RGGB,
    },
};

static const esp_cam_sensor_format_t gc4023_format = {
    .name = "MIPI_2lane_24Minput_RAW10_1920x1080_25fps",
    .format = ESP_CAM_SENSOR_PIXFORMAT_RAW10,
    .port = ESP_CAM_SENSOR_MIPI_CSI,
    .xclk = 24000000,
    .width = 1920,
    .height = 1080,
    .regs = gc4023_1920x1080_25fps_regs,
    .regs_size = sizeof(gc4023_1920x1080_25fps_regs) / sizeof(gc4023_1920x1080_25fps_regs[0]),
    .fps = 25,
    .isp_info = &gc4023_isp_info,
    .mipi_info = {
        .mipi_clk = 720000000,
        .lane_num = 2,
        .line_sync_en = false,
    },
};

typedef struct {
    uint32_t exposure;  // lines
    uint32_t gain_idx;
} gc4023_priv_t;

static esp_err_t gc4023_read(esp_cam_sensor_device_t *dev, uint16_t reg, uint8_t *val)
{
    return esp_sccb_transmit_receive_reg_a16v8(dev->sccb_handle, reg, val);
}

static esp_err_t gc4023_write(esp_cam_sensor_device_t *dev, uint16_t reg, uint8_t val)
{
    return esp_sccb_transmit_reg_a16v8(dev->sccb_handle, reg, val);
}

static esp_err_t gc4023_set_exposure(esp_cam_sensor_device_t *dev, uint32_t lines)
{
    gc4023_priv_t *priv = dev->priv;
    lines = MIN(MAX(lines, GC4023_EXP_MIN), GC4023_EXP_MAX);
    // Group hold (0x031d) so both exposure bytes apply in the same frame
    ESP_RETURN_ON_ERROR(gc4023_write(dev, 0x031d, 0x2d), TAG, "write exposure failed");
    ESP_RETURN_ON_ERROR(gc4023_write(dev, 0x0202, lines >> 8), TAG, "write exposure failed");
    ESP_RETURN_ON_ERROR(gc4023_write(dev, 0x0203, lines & 0xFF), TAG, "write exposure failed");
    ESP_RETURN_ON_ERROR(gc4023_write(dev, 0x031d, 0x28), TAG, "write exposure failed");
    priv->exposure = lines;
    return ESP_OK;
}

// Write order follows HiSilicon cmos_gains_update; 0x031d is group hold so 0x0614/0x0615 apply in the same frame
static esp_err_t gc4023_set_gain(esp_cam_sensor_device_t *dev, uint32_t idx)
{
    gc4023_priv_t *priv = dev->priv;
    idx = MIN(idx, 24);
    const uint8_t *g = gc4023_gain_regs[idx];
    const uint16_t regs[][2] = {
        {0x02cd, g[0]}, {0x031d, 0x2d}, {0x0614, g[1]}, {0x0615, g[2]}, {0x031d, 0x28},
        {0x0218, g[3]}, {0x1467, g[4]}, {0x1468, g[5]}, {0x00b8, g[6]}, {0x00b9, g[7]},
        {0x0064, 0x01}, {0x0065, 0x00},  // digital gain fixed at 1x
        {0x1466, g[8]},
    };
    for (int i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
        ESP_RETURN_ON_ERROR(gc4023_write(dev, regs[i][0], regs[i][1]), TAG, "write gain failed");
    }
    priv->gain_idx = idx;
    return ESP_OK;
}

static esp_err_t gc4023_query_para_desc(esp_cam_sensor_device_t *dev, esp_cam_sensor_param_desc_t *qdesc)
{
    switch (qdesc->id) {
    case ESP_CAM_SENSOR_EXPOSURE_VAL:
        qdesc->type = ESP_CAM_SENSOR_PARAM_TYPE_NUMBER;
        qdesc->number.minimum = GC4023_EXP_MIN;
        qdesc->number.maximum = GC4023_EXP_MAX;
        qdesc->number.step = 1;
        qdesc->default_value = GC4023_EXP_DEF;
        return ESP_OK;
    case ESP_CAM_SENSOR_EXPOSURE_US:
        qdesc->type = ESP_CAM_SENSOR_PARAM_TYPE_NUMBER;
        qdesc->number.minimum = MAX(1, LINES_TO_US(GC4023_EXP_MIN) / EXPOSURE_V4L2_UNIT_US);
        qdesc->number.maximum = LINES_TO_US(GC4023_EXP_MAX) / EXPOSURE_V4L2_UNIT_US;
        qdesc->number.step = 1;
        qdesc->default_value = LINES_TO_US(GC4023_EXP_DEF) / EXPOSURE_V4L2_UNIT_US;
        return ESP_OK;
    case ESP_CAM_SENSOR_GAIN:
        qdesc->type = ESP_CAM_SENSOR_PARAM_TYPE_ENUMERATION;
        qdesc->enumeration.count = 25;
        qdesc->enumeration.elements = gc4023_gain_x1000;
        qdesc->default_value = 0;
        return ESP_OK;
    case ESP_CAM_SENSOR_GROUP_EXP_GAIN:
        qdesc->type = ESP_CAM_SENSOR_PARAM_TYPE_U8;
        qdesc->u8.size = sizeof(esp_cam_sensor_gh_exp_gain_t);
        return ESP_OK;
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

static esp_err_t gc4023_get_para_value(esp_cam_sensor_device_t *dev, uint32_t id, void *arg, size_t size)
{
    gc4023_priv_t *priv = dev->priv;
    switch (id) {
    case ESP_CAM_SENSOR_EXPOSURE_VAL:
        *(uint32_t *)arg = priv->exposure;
        return ESP_OK;
    case ESP_CAM_SENSOR_GAIN:
        *(uint32_t *)arg = priv->gain_idx;
        return ESP_OK;
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}

static esp_err_t gc4023_set_para_value(esp_cam_sensor_device_t *dev, uint32_t id, const void *arg, size_t size)
{
    switch (id) {
    case ESP_CAM_SENSOR_EXPOSURE_VAL:
        return gc4023_set_exposure(dev, *(uint32_t *)arg);
    case ESP_CAM_SENSOR_EXPOSURE_US:
        return gc4023_set_exposure(dev, US_TO_LINES(*(uint32_t *)arg * EXPOSURE_V4L2_UNIT_US));
    case ESP_CAM_SENSOR_GAIN:
        return gc4023_set_gain(dev, *(uint32_t *)arg);
    case ESP_CAM_SENSOR_GROUP_EXP_GAIN: {
        const esp_cam_sensor_gh_exp_gain_t *v = arg;
        uint32_t lines = v->exposure_us ? US_TO_LINES(v->exposure_us * EXPOSURE_V4L2_UNIT_US) : v->exposure_val;
        ESP_RETURN_ON_ERROR(gc4023_set_exposure(dev, lines), TAG, "set exposure failed");
        return gc4023_set_gain(dev, v->gain_index);
    }
    default:
        ESP_LOGE(TAG, "set id=%" PRIx32 " is not supported", id);
        return ESP_ERR_INVALID_ARG;
    }
}

static esp_err_t gc4023_query_support_formats(esp_cam_sensor_device_t *dev, esp_cam_sensor_format_array_t *formats)
{
    formats->count = 1;
    formats->format_array = &gc4023_format;
    return ESP_OK;
}

static esp_err_t gc4023_query_support_capability(esp_cam_sensor_device_t *dev, esp_cam_sensor_capability_t *sensor_cap)
{
    sensor_cap->fmt_raw = 1;
    return ESP_OK;
}

static esp_err_t gc4023_set_format(esp_cam_sensor_device_t *dev, const esp_cam_sensor_format_t *format)
{
    // only one format, used for NULL or anything else
    const gc4023_reginfo_t *regs = gc4023_1920x1080_25fps_regs;
    for (int i = 0; regs[i].reg != GC4023_REG_END; i++) {
        if (regs[i].reg == GC4023_REG_DELAY) {
            vTaskDelay(pdMS_TO_TICKS(regs[i].val));
            continue;
        }
        ESP_RETURN_ON_ERROR(gc4023_write(dev, regs[i].reg, regs[i].val), TAG, "write init regs failed");
    }
    dev->cur_format = &gc4023_format;
    ESP_RETURN_ON_ERROR(gc4023_set_exposure(dev, GC4023_EXP_DEF), TAG, "set exposure failed");
    return gc4023_set_gain(dev, 0);
}

static esp_err_t gc4023_get_format(esp_cam_sensor_device_t *dev, esp_cam_sensor_format_t *format)
{
    memcpy(format, &gc4023_format, sizeof(esp_cam_sensor_format_t));
    return ESP_OK;
}

static esp_err_t gc4023_get_id(esp_cam_sensor_device_t *dev, esp_cam_sensor_id_t *id)
{
    uint8_t high = 0, low = 0;
    ESP_RETURN_ON_ERROR(gc4023_read(dev, 0x03f0, &high), TAG, "read id failed");
    ESP_RETURN_ON_ERROR(gc4023_read(dev, 0x03f1, &low), TAG, "read id failed");
    id->midh = 0;
    id->midl = 0;
    id->pid = (high << 8) | low;
    return ESP_OK;
}

static esp_err_t gc4023_priv_ioctl(esp_cam_sensor_device_t *dev, uint32_t cmd, void *arg)
{
    esp_cam_sensor_reg_val_t *reg = arg;
    switch (cmd) {
    case ESP_CAM_SENSOR_IOC_HW_RESET:
        if (dev->reset_pin >= 0) {
            gpio_set_level(dev->reset_pin, 0);
            vTaskDelay(pdMS_TO_TICKS(10));
            gpio_set_level(dev->reset_pin, 1);
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        return ESP_OK;
    case ESP_CAM_SENSOR_IOC_S_STREAM: {
        int enable = *(int *)arg;
        // init table writes 0x08 (no stream), 0x09 starts streaming
        ESP_RETURN_ON_ERROR(gc4023_write(dev, 0x0100, enable ? 0x09 : 0x08), TAG, "set stream failed");
        dev->stream_status = enable;
        ESP_LOGI(TAG, "Stream=%d", enable);
        return ESP_OK;
    }
    case ESP_CAM_SENSOR_IOC_S_REG:
        return gc4023_write(dev, reg->regaddr, reg->value);
    case ESP_CAM_SENSOR_IOC_G_REG: {
        uint8_t val = 0;
        esp_err_t ret = gc4023_read(dev, reg->regaddr, &val);
        reg->value = val;
        return ret;
    }
    case ESP_CAM_SENSOR_IOC_G_CHIP_ID:
        return gc4023_get_id(dev, arg);
    default:
        return ESP_OK;
    }
}

static esp_err_t gc4023_delete(esp_cam_sensor_device_t *dev)
{
    if (dev->reset_pin >= 0) {
        gpio_set_level(dev->reset_pin, 0);
    }
    free(dev->priv);
    free(dev);
    return ESP_OK;
}

static const esp_cam_sensor_ops_t gc4023_ops = {
    .query_para_desc = gc4023_query_para_desc,
    .get_para_value = gc4023_get_para_value,
    .set_para_value = gc4023_set_para_value,
    .query_support_formats = gc4023_query_support_formats,
    .query_support_capability = gc4023_query_support_capability,
    .set_format = gc4023_set_format,
    .get_format = gc4023_get_format,
    .priv_ioctl = gc4023_priv_ioctl,
    .del = gc4023_delete,
};

esp_cam_sensor_device_t *gc4023_detect(esp_cam_sensor_config_t *config)
{
    esp_cam_sensor_device_t *dev = calloc(1, sizeof(esp_cam_sensor_device_t));
    gc4023_priv_t *priv = calloc(1, sizeof(gc4023_priv_t));
    if (dev == NULL || priv == NULL) {
        free(dev);
        free(priv);
        return NULL;
    }
    dev->name = (char *)GC4023_SENSOR_NAME;
    dev->sccb_handle = config->sccb_handle;
    dev->xclk_pin = config->xclk_pin;
    dev->reset_pin = config->reset_pin;
    dev->pwdn_pin = config->pwdn_pin;
    dev->sensor_port = config->sensor_port;
    dev->ops = &gc4023_ops;
    dev->priv = priv;
    dev->cur_format = &gc4023_format;

    // Reset goes to CSI_RSTN through a diode, high = release reset (board manager has already started MCLK)
    if (dev->reset_pin >= 0) {
        gpio_config_t conf = {
            .pin_bit_mask = 1ULL << dev->reset_pin,
            .mode = GPIO_MODE_OUTPUT,
        };
        gpio_config(&conf);
        gpio_set_level(dev->reset_pin, 1);
    }
    vTaskDelay(pdMS_TO_TICKS(20));

    if (gc4023_get_id(dev, &dev->id) != ESP_OK || dev->id.pid != GC4023_PID) {
        ESP_LOGE(TAG, "Camera sensor is not GC4023, PID=0x%x", dev->id.pid);
        gc4023_delete(dev);
        return NULL;
    }
    ESP_LOGI(TAG, "Detected Camera sensor PID=0x%x", dev->id.pid);
    return dev;
}

ESP_CAM_SENSOR_DETECT_FN(gc4023_detect, ESP_CAM_SENSOR_MIPI_CSI, GC4023_SCCB_ADDR)
{
    ((esp_cam_sensor_config_t *)config)->sensor_port = ESP_CAM_SENSOR_MIPI_CSI;
    return gc4023_detect(config);
}
