/* SPDX-License-Identifier: MIT
 * Explicit developer benchmark only. No libm replacement or guest ABI change.
 */
#include <math.h>
#include <stdbool.h>
#include "driver/cordic.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

enum { POINTS = 64, REPEATS = 80 };
static volatile float result_sink;

static __attribute__((noinline)) void cpu_sincos(const float *angles, float *cs, float *sn) {
    for (unsigned i = 0; i < POINTS; ++i) { cs[i] = cosf(angles[i]); sn[i] = sinf(angles[i]); }
}

static bool hw_sincos(cordic_engine_handle_t engine, const float *angles,
                       float *cs, float *sn, unsigned batch, unsigned iterations) {
    cordic_calculate_config_t cfg = {
        .function = ESP_CORDIC_FUNC_COS, .iq_format = ESP_CORDIC_FORMAT_Q31,
        .iteration_count = iterations,
    };
    /* At most eight points per critical section. Include float conversion
     * and the driver's locking/configuration in the measured cost. Angles
     * are already in [-pi, pi), as in the voxel camera's wrapped yaw. */
    uint32_t input[8], cosine[8], sine[8];
    cordic_input_buffer_desc_t in = {.p_data_arg1 = input};
    cordic_output_buffer_desc_t out = {.p_data_res1 = cosine, .p_data_res2 = sine};
    for (unsigned i = 0; i < POINTS; i += batch) {
        for (unsigned j = 0; j < batch; ++j)
            input[j] = cordic_convert_float_to_fixed(angles[i+j] * (1.0f / 3.14159265358979323846f), cfg.iq_format);
        if (cordic_calculate_polling(engine, &cfg, &in, &out, batch) != ESP_OK) return false;
        for (unsigned j = 0; j < batch; ++j) {
            cs[i+j] = cordic_convert_fixed_to_float(cosine[j], cfg.iq_format);
            sn[i+j] = cordic_convert_fixed_to_float(sine[j], cfg.iq_format);
        }
    }
    return true;
}

bool pxa_render_cordic_test(void) {
    cordic_engine_handle_t engine = NULL;
    const cordic_engine_config_t engine_cfg = {0};
    if (cordic_acquire_engine(&engine_cfg, &engine) != ESP_OK) return false;
    bool passed = false;
    float angles[POINTS], reference_cos[POINTS], reference_sin[POINTS], cs[POINTS], sn[POINTS];
    for (unsigned i = 0; i < POINTS; ++i)
        angles[i] = ((float)i - POINTS / 2) * (6.28318530717958647692f / POINTS);
    cpu_sincos(angles, reference_cos, reference_sin);
    const unsigned batches[] = {1, 2, 8};
    for (unsigned iter = 16; iter <= 24; iter += 8)
        for (unsigned b = 0; b < sizeof(batches)/sizeof(batches[0]); ++b) {
            const unsigned batch = batches[b];
            int64_t elapsed[2] = {0};
            float error = 0;
            for (unsigned r = 0; r < REPEATS; ++r)
                for (unsigned order = 0; order < 2; ++order) {
                    unsigned hw = (r + order) & 1;
                    int64_t start = esp_timer_get_time();
                    if (hw) {
                        if (!hw_sincos(engine, angles, cs, sn, batch, iter)) goto cleanup;
                    } else cpu_sincos(angles, cs, sn);
                    elapsed[hw] += esp_timer_get_time() - start;
                    result_sink = cs[r % POINTS] + sn[r % POINTS];
                    if (hw)
                        for (unsigned i = 0; i < POINTS; ++i) {
                            error = fmaxf(error, fabsf(cs[i] - reference_cos[i]));
                            error = fmaxf(error, fabsf(sn[i] - reference_sin[i]));
                        }
                }
            ESP_LOGI("render_accel", "sincos 64 pairs core=%d iterations=%u batch=%u libm=%lld CORDIC+convert=%lld us max_error_ppm=%u",
                     xPortGetCoreID(), iter, batch, elapsed[0]/REPEATS, elapsed[1]/REPEATS,
                     (unsigned)ceilf(error * 1000000));
            if (!(error <= (iter == 16 ? 0.00005f : 0.000002f))) goto cleanup;
        }

    /* The native UI uses integer degrees and a LUT, not float libm. */
    int64_t start = esp_timer_get_time();
    int32_t sum = 0;
    for (unsigned r = 0; r < REPEATS; ++r)
        for (int i = 0; i < POINTS; ++i)
            sum += lv_trigo_sin(i * 5 - 160) + lv_trigo_cos(i * 5 - 160);
    result_sink = (float)sum;
    ESP_LOGI("render_accel", "LVGL LUT 64 integer sincos pairs core=%d time=%lld us (different precision/domain)",
             xPortGetCoreID(), (esp_timer_get_time() - start)/REPEATS);

    cordic_calculate_config_t cfg = {
        .function = ESP_CORDIC_FUNC_SQUARE_ROOT, .iq_format = ESP_CORDIC_FORMAT_Q31, .iteration_count = 12,
    };
    uint32_t input[8], output[8];
    cordic_input_buffer_desc_t in = {.p_data_arg1 = input};
    cordic_output_buffer_desc_t out = {.p_data_res1 = output};
    int64_t elapsed[2] = {0};
    float error = 0;
    /* CORDIC sqrt has a restricted convergence interval; this is not a
     * replacement for sqrtf over arbitrary magnitudes, zeros or negatives. */
    for (unsigned i = 0; i < POINTS; ++i) angles[i] = 0.125f + (float)i / 80.0f;
    for (unsigned r = 0; r < REPEATS; ++r)
        for (unsigned order = 0; order < 2; ++order) {
            unsigned hw = (r + order) & 1;
            start = esp_timer_get_time();
            if (hw) {
                for (unsigned i = 0; i < POINTS; i += 8) {
                    for (unsigned j = 0; j < 8; ++j) input[j] = cordic_convert_float_to_fixed(angles[i+j], cfg.iq_format);
                    if (cordic_calculate_polling(engine, &cfg, &in, &out, 8) != ESP_OK) goto cleanup;
                    for (unsigned j = 0; j < 8; ++j) cs[i+j] = cordic_convert_fixed_to_float(output[j], cfg.iq_format);
                }
            } else for (unsigned i = 0; i < POINTS; ++i) cs[i] = sqrtf(angles[i]);
            elapsed[hw] += esp_timer_get_time() - start;
            result_sink = cs[r % POINTS];
            if (hw) for (unsigned i = 0; i < POINTS; ++i) error = fmaxf(error, fabsf(cs[i] - sqrtf(angles[i])));
        }
    ESP_LOGI("render_accel", "sqrt 64 points core=%d FPU/libm=%lld CORDIC+convert=%lld us max_error_ppm=%u",
             xPortGetCoreID(), elapsed[0]/REPEATS, elapsed[1]/REPEATS, (unsigned)ceilf(error * 1000000));
    passed = error <= 0.00001f;
cleanup:
    if (cordic_release_engine(engine) != ESP_OK) passed = false;
    if (!passed) ESP_LOGE("render_accel", "CORDIC comparison FAILED");
    return passed;
}
