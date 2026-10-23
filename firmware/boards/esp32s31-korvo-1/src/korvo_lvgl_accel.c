#include "korvo_lvgl_accel.h"
#include "src/draw/sw/blend/lv_draw_sw_blend_private.h"
#include "src/draw/sw/blend/lv_draw_sw_blend_to_rgb565.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

static const char *TAG = "korvo_lvgl";
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
#define KORVO_CPU_STATS_VALID 1
#else
#define KORVO_CPU_STATS_VALID 0
#endif
#if PXA_KORVO_LVGL_DIAGNOSTICS
static atomic_uint image_calls, argb_calls, premult_calls, pie_calls, pie_pixels, core0_fallbacks;
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
static uint32_t previous_idle[2];
#endif
static int64_t previous_us;
#define COUNT(counter, value) atomic_fetch_add_explicit(&(counter), (value), memory_order_relaxed)
#else
#define COUNT(counter, value) ((void)0)
#endif

void __real_lv_draw_sw_blend_image_to_rgb565(lv_draw_sw_blend_image_dsc_t *dsc);

#if PXA_KORVO_LVGL_PIE
void korvo_pie_argb_rows(uint16_t *, const uint8_t *, int, int, int, int, uint32_t, void *);

/* Keep two workers on separate cores. S31 PIE permanently pins its user to
 * Core1; letting both workers use it would silently serialize all SW drawing.
 * Only the Core1 worker (or an already pinned Core1 caller) uses this kernel. */
BaseType_t __real_xTaskCreatePinnedToCore(TaskFunction_t, const char *const,
    const configSTACK_DEPTH_TYPE, void *const, UBaseType_t, TaskHandle_t *const, const BaseType_t);
BaseType_t __wrap_xTaskCreatePinnedToCore(TaskFunction_t code, const char *const name,
    const configSTACK_DEPTH_TYPE depth, void *const argument, UBaseType_t priority,
    TaskHandle_t *const created, const BaseType_t core)
{
    static unsigned worker;
    bool sw = name && strcmp(name, "swdraw") == 0;
    BaseType_t target = sw ? (worker++ & 1u) : core;
    BaseType_t result = __real_xTaskCreatePinnedToCore(code, name, depth, argument,
                                                     priority, created, target);
    if (sw && result == pdPASS)
        ESP_LOGI(TAG, "DRAW_AFFINITY worker=%u core=%d stack_bytes=%u", worker - 1,
                 (int)target, (unsigned)depth);
    return result;
}

static bool try_argb(lv_draw_sw_blend_image_dsc_t *dsc)
{
    if (dsc->src_color_format != LV_COLOR_FORMAT_ARGB8888 ||
        dsc->blend_mode != LV_BLEND_MODE_NORMAL || dsc->mask_buf ||
        dsc->dest_w < 8 || dsc->dest_h <= 0 ||
        dsc->src_stride < dsc->dest_w * 4 || dsc->dest_stride < dsc->dest_w * 2)
        return false;
    /* Query affinity, not the current CPU: an unpinned task can migrate
     * between a CPU check and its first PIE instruction. */
    if (xTaskGetCoreID(NULL) != 1) {
        COUNT(core0_fallbacks, 1);
        return false;
    }
    uintptr_t src = (uintptr_t)dsc->src_buf, dst = (uintptr_t)dsc->dest_buf;
    uintptr_t src_end = src + (dsc->dest_h - 1) * dsc->src_stride + dsc->dest_w * 4;
    uintptr_t dst_end = dst + (dsc->dest_h - 1) * dsc->dest_stride + dsc->dest_w * 2;
    if (src < dst_end && dst < src_end) return false;
    if (!dsc->opa) return true;
    int width = dsc->dest_w & ~7;
    korvo_pie_argb_rows(dsc->dest_buf, dsc->src_buf, width, dsc->dest_h,
                       dsc->dest_stride, dsc->src_stride, dsc->opa, NULL);
    for (int y = 0; y < dsc->dest_h; ++y) {
        uint16_t *out = (uint16_t *)((uint8_t *)dsc->dest_buf + y * dsc->dest_stride);
        const uint8_t *in = (const uint8_t *)dsc->src_buf + y * dsc->src_stride;
        for (int x = width; x < dsc->dest_w; ++x) {
            uint32_t alpha = in[x * 4 + 3];
            if (!alpha) continue;
            if (dsc->opa < LV_OPA_MAX) alpha = LV_OPA_MIX2(alpha, dsc->opa);
            uint16_t fg = LV_COLOR_24_TO_16(in + x * 4);
            out[x] = lv_color_16_16_mix_inlined(fg, out[x], alpha);
        }
    }
    COUNT(pie_calls, 1);
    COUNT(pie_pixels, width * dsc->dest_h);
    return true;
}
#endif

void __wrap_lv_draw_sw_blend_image_to_rgb565(lv_draw_sw_blend_image_dsc_t *dsc)
{
    COUNT(image_calls, 1);
    if (dsc->src_color_format == LV_COLOR_FORMAT_ARGB8888) COUNT(argb_calls, 1);
    if (dsc->src_color_format == LV_COLOR_FORMAT_ARGB8888_PREMULTIPLIED) COUNT(premult_calls, 1);
#if PXA_KORVO_LVGL_PIE
    if (try_argb(dsc)) return;
#endif
    __real_lv_draw_sw_blend_image_to_rgb565(dsc);
}

#if PXA_KORVO_LVGL_SELF_TEST
void korvo_lvgl_accel_self_test(void);
#endif
void korvo_lvgl_accel_init(void)
{
    ESP_LOGI(TAG, "ACCEL pie=%d diagnostics=%d self_test=%d workers=%d",
             PXA_KORVO_LVGL_PIE, PXA_KORVO_LVGL_DIAGNOSTICS,
             PXA_KORVO_LVGL_SELF_TEST, LV_DRAW_SW_DRAW_UNIT_CNT);
#if PXA_KORVO_LVGL_SELF_TEST
    korvo_lvgl_accel_self_test();
#endif
#if PXA_KORVO_LVGL_DIAGNOSTICS && CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    previous_idle[0] = ulTaskGetIdleRunTimeCounterForCore(0);
    previous_idle[1] = ulTaskGetIdleRunTimeCounterForCore(1);
    previous_us = esp_timer_get_time();
#endif
}

void korvo_lvgl_accel_log(void)
{
#if PXA_KORVO_LVGL_DIAGNOSTICS
    int64_t now = esp_timer_get_time();
    unsigned busy[2] = {0, 0};
    int64_t elapsed = now - previous_us;
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    for (int core = 0; core < 2; ++core) {
        uint32_t idle = ulTaskGetIdleRunTimeCounterForCore(core);
        uint64_t percent = elapsed > 0 ? (uint64_t)(idle - previous_idle[core]) * 100 / elapsed : 100;
        busy[core] = percent > 100 ? 0 : 100 - percent;
        previous_idle[core] = idle;
    }
#endif
    previous_us = now;
    ESP_LOGI(TAG, "ACCEL_SAMPLE device_us=%lld elapsed_us=%lld cpu_valid=%d core0=%u core1=%u cpu=%u images=%u argb=%u premult=%u pie_calls=%u pie_pixels=%u core0_fallbacks=%u",
        (long long)now, (long long)elapsed, KORVO_CPU_STATS_VALID,
        busy[0], busy[1], (busy[0] + busy[1]) / 2,
        atomic_exchange(&image_calls, 0), atomic_exchange(&argb_calls, 0),
        atomic_exchange(&premult_calls, 0), atomic_exchange(&pie_calls, 0),
        atomic_exchange(&pie_pixels, 0), atomic_exchange(&core0_fallbacks, 0));
#endif
}
