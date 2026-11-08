/* SPDX-License-Identifier: MIT */
#include "pxa_render_accel.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint16_t reference_mix(uint16_t bg, uint16_t fg, unsigned opa) {
    unsigned m = (opa + 4) >> 3, inv = 32 - m;
    return (uint16_t)(((((bg >> 11) * inv + (fg >> 11) * m) >> 5) << 11) |
        (((((bg >> 5) & 63) * inv + ((fg >> 5) & 63) * m) >> 5) << 5) |
        (((bg & 31) * inv + (fg & 31) * m) >> 5));
}

int main(void) {
    _Alignas(64) unsigned char src[512], dst[2048];
    for (unsigned i = 0; i < sizeof(src); ++i) src[i] = (unsigned char)(i * 37u + 9u);
    for (unsigned input = 0; input < 17; ++input) {
        for (unsigned output = 0; output < 17; ++output) {
            for (unsigned n = 0; n < 129; ++n) {
                memset(dst, 0x5a, sizeof(dst));
                pxa_render_swap_rgb565(dst + output, src + input, n);
                for (unsigned i = 0; i < sizeof(dst); ++i)
                    assert(dst[i] == (i >= output && i < output + n*2
                        ? src[input + ((i-output)^1u)] : 0x5a));
                pxa_render_swap_rgb565(dst + output, dst + output, n);
                assert(memcmp(dst + output, src + input, n*2) == 0);
                uint32_t first = 987, last = 654;
                assert(!pxa_render_diff_rgb565(dst + output, src + input, n, &first, &last));
                assert(first == 987 && last == 654);
                if (n) {
                    dst[output + (n/3)*2] ^= 0x40;
                    dst[output + (n-1)*2+1] ^= 0x01;
                    assert(pxa_render_diff_rgb565(dst + output, src + input, n, &first, &last));
                    assert(first == n/3 && last == n-1);
                }
                for (unsigned scale = 2; scale <= 4; scale += 2)
                    for (unsigned swap = 0; swap <= 1; ++swap) {
                        memset(dst, 0x5a, sizeof(dst));
                        pxa_render_expand_rgb565(dst + output, src + input, n, scale, swap);
                        for (unsigned i = 0; i < sizeof(dst); ++i) {
                            unsigned x = i-output;
                            unsigned at = input + (x/(scale*2))*2 + ((x&1u)^swap);
                            assert(dst[i] == (i >= output && i < output + n*scale*2 ? src[at] : 0x5a));
                        }
                    }
            }
        }
    }
    for (unsigned offset = 0; offset < 16; ++offset)
        for (unsigned n = 0; n < 129; ++n) {
            uint16_t *p = (uint16_t *)dst;
            memset(dst, 0x5a, sizeof(dst));
            pxa_render_fill_rgb565(p+offset, n, 0xad37);
            for (unsigned i = 0; i < sizeof(dst)/2; ++i)
                assert(p[i] == (i >= offset && i < offset+n ? 0xad37 : 0x5a5a));
        }
    const uint16_t colors[] = {0, 0xffff, 0xf800, 0x07e0, 0x001f, 0x172f, 0xa37c};
    for (unsigned opacity = 0; opacity < 256; ++opacity)
        for (unsigned c = 0; c < sizeof(colors)/sizeof(colors[0]); ++c)
            for (unsigned offset = 0; offset < 8; ++offset)
                for (unsigned n = 0; n <= 129; n += 3) {
                    uint16_t *p = (uint16_t *)dst;
                    for (unsigned i = 0; i < 256; ++i) p[i] = (uint16_t)(i * 32771u + opacity * 137u);
                    pxa_render_mix_rgb565(p + offset, n, colors[c], opacity);
                    for (unsigned i = 0; i < 256; ++i) {
                        uint16_t bg = (uint16_t)(i * 32771u + opacity * 137u);
                        assert(p[i] == (i >= offset && i < offset + n ? reference_mix(bg, colors[c], opacity) : bg));
                    }
                }
    puts("RGB565 fill/mix/swap/nearest expansion: opacity, offsets, tails, in-place swap and guards passed");
    return 0;
}
