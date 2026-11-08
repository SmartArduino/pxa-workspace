// SPDX-FileCopyrightText: 2026 SmartArduino
// SPDX-FileCopyrightText: 2001-2025 Shenshu Tech. Co., Ltd.
// SPDX-License-Identifier: Apache-2.0 AND MIT
// ESP32 adaptation: SmartArduino/ESPP4-A 1e981d9f159a1afb30bd5e41b0137344ce3916a6.
// Sensor register/gain data: OpenIPC/openhisilicon sensor libraries (MIT),
// c0d8112da51c8e9f6215a2208bdeaefc1b6201bf; see NOTICE and LICENSE.MIT.
#pragma once

#include "esp_cam_sensor_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GC4023_SCCB_ADDR   0x29
#define GC4023_PID         0x4023
#define GC4023_SENSOR_NAME "GC4023"

esp_cam_sensor_device_t *gc4023_detect(esp_cam_sensor_config_t *config);

#ifdef __cplusplus
}
#endif
