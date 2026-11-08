/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Native RGB565 primitives. Inputs and outputs must not overlap, except that
 * swap supports src == dst. No padding is read or written. Scalar fallbacks
 * preserve the same bytes on other targets, on S31 core 0 and in unpinned
 * tasks. Call from task context; SIMD requires a task pinned to S31 core 1. */
void pxa_render_fill_rgb565(uint16_t *dst, uint32_t count, uint16_t color);
/* Constant-color blend, with LVGL's RGB565 opacity rounding: (opa + 4) / 8.
 * Destination is updated in place; all 256 opacity values are supported. */
void pxa_render_mix_rgb565(uint16_t *dst, uint32_t count, uint16_t color, uint8_t opa);
bool pxa_render_simd_available(void);
void pxa_render_swap_rgb565(void *dst, const void *src, uint32_t count);
void pxa_render_expand_rgb565(void *dst, const void *src, uint32_t count,
                             unsigned scale, bool swap);
/* Inclusive bounds of changed pixels; outputs are untouched for equal rows. */
bool pxa_render_diff_rgb565(const void *a, const void *b, uint32_t count,
                           uint32_t *first, uint32_t *last);

/* Full-width, cache-line-aligned native RGB565 bands. False leaves the caller
 * responsible for rendering the entire request, including after a DMA error.
 * The destination must remain exclusively owned until this call returns. */
bool pxa_render_fill_bands(void *context, uint16_t *pixels, uint32_t stride,
                          uint16_t width, uint16_t height, const uint16_t *tops,
                          const uint16_t *bottoms, const uint16_t *colors,
                          uint8_t count);
uint32_t pxa_render_ppa_fill_count(void);

/* Bit-exact 1:1 INDEX8 + RGB565 palette resolve. Padded/unaligned pictures,
 * small transfers and unsupported hardware return false for CPU rendering.
 * The palette and source must remain immutable, destination exclusive. */
bool pxa_render_index8_rgb565(uint16_t *dst, const uint8_t *src,
                             const uint16_t *palette, uint16_t width,
                             uint16_t height);
uint32_t pxa_render_ppa_index_count(void);

/* Optional on-device differential tests; never changes display state. */
bool pxa_render_accel_self_test(void);

#ifdef __cplusplus
}
#endif
