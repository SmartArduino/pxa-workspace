/* SPDX-License-Identifier: MIT */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
/* LVGL also includes this from lv_conf_internal before declaring its types.
 * An opaque descriptor and integer result keep this hook independent of that
 * include order. The implementation returns LV_RESULT_OK/INVALID. */
int pxa_render_lvgl_fill(void *dsc);
#define LV_DRAW_SW_COLOR_BLEND_TO_RGB565(dsc) pxa_render_lvgl_fill(dsc)
#define LV_DRAW_SW_COLOR_BLEND_TO_RGB565_WITH_OPA(dsc) pxa_render_lvgl_fill(dsc)
#ifdef __cplusplus
}
#endif
