/* SPDX-License-Identifier: MIT */
#include "pxa_render_accel.h"
#include "lvgl.h"
#include "sdkconfig.h"
#if CONFIG_PXA_RENDER_ACCEL_SELF_TEST
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
static TaskHandle_t fallback_task;
void pxa_render_lvgl_test_fallback(bool enabled) {
    __atomic_store_n(&fallback_task, enabled ? xTaskGetCurrentTaskHandle() : NULL, __ATOMIC_RELEASE);
}
#endif
#if LV_USE_DRAW_SW
#include "src/draw/sw/blend/lv_draw_sw_blend_private.h"
#endif

int pxa_render_lvgl_fill(void *opaque) {
#if LV_USE_DRAW_SW
#if CONFIG_PXA_RENDER_ACCEL_SELF_TEST
    if (__atomic_load_n(&fallback_task, __ATOMIC_ACQUIRE) == xTaskGetCurrentTaskHandle()) return LV_RESULT_INVALID;
#endif
    lv_draw_sw_blend_fill_dsc_t *dsc = opaque;
    if (!pxa_render_simd_available() || !dsc || dsc->dest_w < 32 || dsc->dest_h <= 0 || dsc->mask_buf ||
        (dsc->opa < LV_OPA_MAX && dsc->dest_w < 64)) return LV_RESULT_INVALID;
    uint16_t color = lv_color_to_u16(dsc->color);
    uint8_t *row = dsc->dest_buf;
    if (dsc->opa < LV_OPA_MAX) {
        if (dsc->dest_stride == dsc->dest_w * 2)
            pxa_render_mix_rgb565((uint16_t *)row, dsc->dest_w * dsc->dest_h, color, dsc->opa);
        else
            for (int y = 0; y < dsc->dest_h; ++y, row += dsc->dest_stride)
                pxa_render_mix_rgb565((uint16_t *)row, dsc->dest_w, color, dsc->opa);
    } else if (dsc->dest_stride == dsc->dest_w * 2) {
        pxa_render_fill_rgb565((uint16_t *)row, dsc->dest_w * dsc->dest_h, color);
    } else {
        for (int y = 0; y < dsc->dest_h; ++y, row += dsc->dest_stride)
            pxa_render_fill_rgb565((uint16_t *)row, dsc->dest_w, color);
    }
    return LV_RESULT_OK;
#else
    (void)opaque;
    return LV_RESULT_INVALID;
#endif
}
