// SPDX-License-Identifier: Apache-2.0
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
int espp4_a_lvgl_fill(void* descriptor);
#define LV_DRAW_SW_COLOR_BLEND_TO_RGB565(dsc) espp4_a_lvgl_fill(dsc)
#ifdef __cplusplus
}
#endif
