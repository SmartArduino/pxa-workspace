/* SPDX-License-Identifier: MIT */
#include "pxa_render_accel.h"
#include <string.h>

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif
#if defined(CONFIG_PXA_RENDER_ACCEL) && CONFIG_PXA_RENDER_ACCEL
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
void pxa_s31_fill16(uint16_t *dst, uint32_t blocks, const uint16_t *color);
void pxa_s31_mix16(uint16_t *dst, uint32_t blocks, const uint16_t *constants,
                   uint32_t red, uint32_t green, uint32_t blue);
void pxa_s31_swap16(void *dst, const void *src, uint32_t blocks);
void pxa_s31_expand16(void *dst, const void *src, uint32_t blocks,
                      unsigned scale, bool swap);
uint32_t pxa_s31_equal_blocks(const void *a, const void *b, uint32_t blocks);
static bool simd_core(void) {
    /* An unpinned task could migrate after the core check. Keep its scalar
     * path rather than letting the PIE trap silently change task affinity. */
    return xPortGetCoreID() == 1 && xTaskGetCoreID(NULL) == 1;
}
#else
static bool simd_core(void) { return false; }
#endif

bool pxa_render_simd_available(void) { return simd_core(); }

void pxa_render_fill_rgb565(uint16_t *dst, uint32_t count, uint16_t color) {
#if defined(CONFIG_PXA_RENDER_ACCEL) && CONFIG_PXA_RENDER_ACCEL
    if (count >= 32 && simd_core()) {
        while (count && ((uintptr_t)dst & 15u)) { *dst++ = color; --count; }
        uint32_t blocks = count / 8;
        if (blocks) pxa_s31_fill16(dst, blocks, &color);
        dst += blocks * 8; count -= blocks * 8;
    }
#else
    (void)simd_core;
#endif
    if (count && ((uintptr_t)dst & 3u)) { *dst++ = color; --count; }
    if (!count) return;
    const uint32_t pair = color | ((uint32_t)color << 16);
    uint32_t *wide = (uint32_t *)__builtin_assume_aligned(dst, 4);
    while (count >= 2) { memcpy(wide++, &pair, 4); count -= 2; }
    if (count) memcpy(wide, &color, 2);
}

static inline uint16_t mix_pixel(uint16_t bg, uint32_t fg, unsigned weight) {
    uint32_t packed = (bg | ((uint32_t)bg << 16)) & 0x07e0f81fu;
    packed = ((((fg - packed) * weight) >> 5) + packed) & 0x07e0f81fu;
    return (uint16_t)(packed | (packed >> 16));
}

void pxa_render_mix_rgb565(uint16_t *dst, uint32_t count, uint16_t color, uint8_t opa) {
    const unsigned weight = ((unsigned)opa + 4) >> 3;
    if (!weight || !count) return;
    if (weight == 32) { pxa_render_fill_rgb565(dst, count, color); return; }
    const uint32_t fg = (color | ((uint32_t)color << 16)) & 0x07e0f81fu;
#if defined(CONFIG_PXA_RENDER_ACCEL) && CONFIG_PXA_RENDER_ACCEL
    if (count >= 64 && simd_core()) {
        while ((uintptr_t)dst & 15u) { *dst = mix_pixel(*dst, fg, weight); ++dst; --count; }
        const uint16_t constants[] = {0xf800, 0x07e0, 0x001f, (uint16_t)(32 - weight)};
        const uint32_t blocks = count / 8;
        pxa_s31_mix16(dst, blocks, constants,
                      ((color & 0xf800u) * weight) >> 5,
                      ((color & 0x07e0u) * weight) >> 5,
                      (color & 0x001fu) * weight);
        dst += blocks * 8; count -= blocks * 8;
    }
#endif
    while (count--) { *dst = mix_pixel(*dst, fg, weight); ++dst; }
}

void pxa_render_swap_rgb565(void *dst, const void *src, uint32_t count) {
    uint8_t *out = (uint8_t *)dst;
    const uint8_t *in = (const uint8_t *)src;
#if defined(CONFIG_PXA_RENDER_ACCEL) && CONFIG_PXA_RENDER_ACCEL
    if (count >= 32 && simd_core() && !((uintptr_t)out & 1u) &&
        !(((uintptr_t)out ^ (uintptr_t)in) & 15u)) {
        while (count && ((uintptr_t)out & 15u)) {
            uint16_t v; memcpy(&v, in, 2); v = __builtin_bswap16(v);
            memcpy(out, &v, 2); in += 2; out += 2; --count;
        }
        uint32_t blocks = count / 8;
        if (blocks) pxa_s31_swap16(out, in, blocks);
        in += blocks * 16; out += blocks * 16; count -= blocks * 8;
    }
#endif
    while (count >= 2) {
        uint32_t v; memcpy(&v, in, 4);
        v = ((v & 0x00ff00ffu) << 8) | ((v >> 8) & 0x00ff00ffu);
        memcpy(out, &v, 4); in += 4; out += 4; count -= 2;
    }
    if (count) { uint16_t v; memcpy(&v, in, 2); v = __builtin_bswap16(v); memcpy(out, &v, 2); }
}

/* Constant scale/swap variants retain the old paired-pixel CPU throughput. */
static inline __attribute__((always_inline)) void expand_words(
    uint32_t *out, const uint32_t *in, uint32_t count, unsigned scale, bool swap) {
    while (count >= 2) {
        uint32_t pair; memcpy(&pair, in++, 4);
        if (swap) pair = ((pair & 0x00ff00ffu) << 8) | ((pair >> 8) & 0x00ff00ffu);
        uint32_t low = (pair & 0xffffu) * 0x00010001u;
        uint32_t high = (pair >> 16) * 0x00010001u;
        memcpy(out++, &low, 4);
        if (scale == 4) memcpy(out++, &low, 4);
        memcpy(out++, &high, 4);
        if (scale == 4) memcpy(out++, &high, 4);
        count -= 2;
    }
    if (count) {
        uint16_t v; memcpy(&v, in, 2);
        if (swap) v = __builtin_bswap16(v);
        uint32_t pair = v | ((uint32_t)v << 16);
        memcpy(out++, &pair, 4);
        if (scale == 4) memcpy(out, &pair, 4);
    }
}

void pxa_render_expand_rgb565(void *dst, const void *src, uint32_t count,
                             unsigned scale, bool swap) {
    uint8_t *out = (uint8_t *)dst;
    const uint8_t *in = (const uint8_t *)src;
    if (scale != 2 && scale != 4) return;
#if defined(CONFIG_PXA_RENDER_ACCEL) && CONFIG_PXA_RENDER_ACCEL
    if (count >= 16 && simd_core() && !((uintptr_t)in & 15u) && !((uintptr_t)out & 15u)) {
        uint32_t blocks = count / 8;
        pxa_s31_expand16(out, in, blocks, scale, swap);
        in += blocks * 16; out += blocks * 16 * scale; count -= blocks * 8;
    }
#endif
    /* Preserve word stores on the CPU fallback. A void/byte pointer alone
     * otherwise makes the RISC-V compiler emit four byte stores per pair. */
    if (!(((uintptr_t)out | (uintptr_t)in) & 3u)) {
        uint32_t *pairs = (uint32_t *)__builtin_assume_aligned(out, 4);
        const uint32_t *pixels = (const uint32_t *)__builtin_assume_aligned(in, 4);
        if (scale == 4) {
            if (swap) expand_words(pairs, pixels, count, 4, true);
            else expand_words(pairs, pixels, count, 4, false);
        } else {
            if (swap) expand_words(pairs, pixels, count, 2, true);
            else expand_words(pairs, pixels, count, 2, false);
        }
        return;
    }
    while (count--) {
        uint16_t v; memcpy(&v, in, 2); in += 2;
        if (swap) v = __builtin_bswap16(v);
        uint32_t pair = v | ((uint32_t)v << 16);
        memcpy(out, &pair, 4);
        if (scale == 4) memcpy(out + 4, &pair, 4);
        out += scale * 2;
    }
}

static bool equal_pixel(const uint8_t *a, const uint8_t *b, uint32_t index) {
    uint16_t x, y;
    memcpy(&x, a + index * 2, 2); memcpy(&y, b + index * 2, 2);
    return x == y;
}
bool pxa_render_diff_rgb565(const void *a, const void *b, uint32_t count,
                           uint32_t *first, uint32_t *last) {
    const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
    uint32_t left = 0;
#if defined(CONFIG_PXA_RENDER_ACCEL) && CONFIG_PXA_RENDER_ACCEL
    if (count >= 16 && simd_core() && !(((uintptr_t)x | (uintptr_t)y) & 15u))
        left = pxa_s31_equal_blocks(x, y, count / 8) * 8;
#endif
    while (left < count && equal_pixel(x, y, left)) ++left;
    if (left == count) return false;
    uint32_t right = count - 1;
    /* The interior cannot enlarge the bounds. Large animated regions usually
     * need only the first and last changed pixel of each row. */
    while (right > left && equal_pixel(x, y, right)) --right;
    *first = left; *last = right;
    return true;
}
