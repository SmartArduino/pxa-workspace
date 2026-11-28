// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// Task context only. Both P4 HP cores support PIE; unpinned tasks retain the
// portable path, so using this helper never changes a caller's affinity.
bool espp4_a_pixels_simd_available(void);
void espp4_a_init_frame_copy(void);
void espp4_a_copy_frame(void* output, const void* input, size_t bytes);
bool espp4_a_copy_video_rows(uint8_t* out, uint32_t stride, const uint8_t* in,
                             uint32_t width, uint32_t height);
void espp4_a_fill_rgb565(uint16_t* output, uint32_t count, uint16_t color);
void espp4_a_expand_rgb565(void* output, const void* input, uint32_t count, unsigned scale);
int espp4_a_lvgl_fill(void* descriptor);
bool espp4_a_fill_bands(void* context, uint16_t* pixels, uint32_t stride,
    uint16_t width, uint16_t height, const uint16_t* tops,
    const uint16_t* bottoms, const uint16_t* colors, uint8_t count);
bool espp4_a_pixels_device_test(void);
#ifdef __cplusplus
}
#endif
