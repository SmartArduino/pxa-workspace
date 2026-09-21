#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "pxa/pxa_surface_transform.h"

static uint16_t swap16(uint16_t value) {
    return (uint16_t)((value << 8) | (value >> 8));
}

static void reference(const uint16_t *source, uint16_t *output,
                      uint16_t width, uint16_t height, uint8_t scale,
                      int byte_swapped) {
    uint16_t y;
    uint16_t x;
    const uint16_t logical_height = (uint16_t)(height * scale);
    for (y = 0; y < height; ++y) {
        for (x = 0; x < width; ++x) {
            uint16_t value = source[(uint32_t)y * width + x];
            uint8_t duplicate_y;
            uint8_t duplicate_x;
            if (!byte_swapped) value = swap16(value);
            for (duplicate_y = 0; duplicate_y < scale; ++duplicate_y) {
                for (duplicate_x = 0; duplicate_x < scale; ++duplicate_x) {
                    const uint16_t logical_x =
                        (uint16_t)(x * scale + duplicate_x);
                    const uint16_t logical_y =
                        (uint16_t)(y * scale + duplicate_y);
                    output[(uint32_t)logical_x * logical_height +
                           logical_height - 1u - logical_y] = value;
                }
            }
        }
    }
}

static void check_scale(uint8_t scale, int byte_swapped) {
    /* Four unique corners plus a non-symmetric center/checker pattern catch
     * mirrored rotation and duplicate-boundary errors. */
    const uint16_t source[12] = {
        0xf800, 0x001f, 0x07e0, 0xffff,
        0x0000, 0x1234, 0xabcd, 0x7bef,
        0xffe0, 0xf81f, 0x07ff, 0x4208,
    };
    uint16_t actual[12 * 16];
    uint16_t expected[12 * 16];
    const uint16_t split = 2;
    memset(actual, 0xa5, sizeof(actual));
    memset(expected, 0x5a, sizeof(expected));
    reference(source, expected, 4, 3, scale, byte_swapped);
    assert(pxa_surface_transform_rgb565_270_columns(
        source, actual, 4, 3, 0, split, scale, byte_swapped != 0));
    assert(pxa_surface_transform_rgb565_270_columns(
        source, actual, 4, 3, split, 4, scale, byte_swapped != 0));
    assert(memcmp(actual, expected,
                  (size_t)12 * scale * scale * sizeof(actual[0])) == 0);
}

static void check_upscale(uint8_t scale) {
    const uint16_t source[8] = {
        0x1001, 0x2002, 0x3003, 0x4004,
        0x5005, 0x6006, 0x7007, 0x8008,
    };
    uint16_t actual[8 * 16];
    uint16_t expected[8 * 16];
    const uint32_t output_stride = 4u * scale;
    memset(actual, 0xa5, sizeof(actual));
    memset(expected, 0xa5, sizeof(expected));
    for (uint16_t y = 0; y < 2; ++y) {
        for (uint16_t x = 0; x < 4; ++x) {
            for (uint8_t dy = 0; dy < scale; ++dy) {
                for (uint8_t dx = 0; dx < scale; ++dx) {
                    expected[((uint32_t)y * scale + dy) * output_stride +
                             (uint32_t)x * scale + dx] = source[y * 4u + x];
                }
            }
        }
    }
    assert(pxa_surface_upscale_rgb565_nearest(
        source, actual, 4, 2, 4, output_stride, scale));
    assert(memcmp(actual, expected,
                  (size_t)8 * scale * scale * sizeof(actual[0])) == 0);
}

static void check_capture_round_trip(void) {
    const uint16_t logical[12] = {
        0xf800, 0x07e0, 0x001f, 0xffff,
        0x0000, 0xffe0, 0xf81f, 0x07ff,
        0x1234, 0x5678, 0x9abc, 0xdef0,
    };
    uint16_t physical[12] = {0};
    uint16_t restored[18] = {0};
    assert(pxa_surface_transform_rgb565_270_columns(
        logical, physical, 4, 3, 0, 4, 1, false));
    assert(pxa_surface_capture_rgb565_270_to_logical(
        physical, restored, 4, 3, 3, 4, true));
    assert(memcmp(logical, restored, sizeof(logical)) == 0);

    memset(restored, 0, sizeof(restored));
    assert(pxa_surface_capture_rgb565_270_to_logical(
        physical, restored, 4, 3, 3, 6, true));
    for (uint16_t y = 0; y < 3; ++y)
        assert(memcmp(logical + y * 4, restored + y * 6,
                      4 * sizeof(uint16_t)) == 0);
}

static void check_alpha_blend(void) {
    uint16_t destination[12] = {
        0x001f, 0x001f, 0x001f, 0x001f,
        0x001f, 0x001f, 0x001f, 0x001f,
        0x001f, 0x001f, 0x001f, 0x001f,
    };
    const uint16_t foreground[6] = {
        0xf800, 0x07e0, 0xffff,
        0xffff, 0xf800, 0x07e0,
    };
    const uint8_t alpha[6] = {0, 255, 128, 255, 0, 255};
    uint16_t clipped[4] = {0, 0, 0, 0};
    assert(pxa_surface_blend_rgb565_a8(
               destination, 4, 3, 4, foreground, alpha, 3, 2, 3, 3,
               1, 1, 255) == 4);
    assert(destination[5] == 0x001f);
    assert(destination[6] == 0x07e0);
    assert(destination[7] == 0x841f);
    assert(destination[9] == 0xffff);
    assert(destination[10] == 0x001f);
    assert(destination[11] == 0x07e0);

    assert(pxa_surface_blend_rgb565_a8(
               clipped, 2, 2, 2, foreground, alpha, 3, 2, 3, 3,
               -1, 1, 255) == 2);
    assert(clipped[2] == 0x07e0 && clipped[3] == 0x8410);
    assert(pxa_surface_blend_rgb565_a8(
               clipped, 2, 2, 2, foreground, alpha, 3, 2, 3, 3,
               0, 0, 0) == 0);
    {
        uint16_t sparse_destination[12];
        const uint16_t sparse_foreground[16] = {
            0xf800, 0xf800, 0xf800, 0xf800,
            0xf800, 0xf800, 0xf800, 0xf800,
            0xf800, 0xf800, 0xf800, 0xf800,
            0xf800, 0xf800, 0xf800, 0xf800,
        };
        const uint8_t sparse_alpha[8] = {0, 0, 0, 0, 0, 0, 0, 255};
        memset(sparse_destination, 0, sizeof(sparse_destination));
        assert(pxa_surface_blend_rgb565_a8(
                   sparse_destination, 4, 3, 4,
                   sparse_foreground + 1, sparse_alpha, 4, 2, 8, 4,
                   0, 1, 255) == 1);
        assert(sparse_destination[11] == 0xf800);
        assert(sparse_destination[7] == 0);
    }
}

static void check_alpha_spans(void) {
    const uint8_t alpha[12] = {
        0, 255, 128, 0, 0, 255,
        255, 255, 0, 0, 128, 0,
    };
    pxa_surface_alpha_span_t spans[4];
    assert(pxa_surface_alpha_spans(alpha, 5, 2, 6, spans, 4) == 3);
    assert(spans[0].x == 1 && spans[0].y == 0 && spans[0].width == 2);
    assert(spans[1].x == 0 && spans[1].y == 1 && spans[1].width == 2);
    assert(spans[2].x == 4 && spans[2].y == 1 && spans[2].width == 1);
    assert(pxa_surface_alpha_spans(alpha, 5, 2, 6, spans, 2) == SIZE_MAX);
    assert(pxa_surface_alpha_spans(alpha, 5, 2, 4, spans, 4) == SIZE_MAX);
    {
        uint16_t colors[12];
        uint16_t full[12];
        uint16_t sparse[12];
        const size_t count = pxa_surface_alpha_spans(
            alpha, 5, 2, 6, spans, 4);
        for (size_t index = 0; index < 12; ++index) {
            colors[index] = 0xf800;
            full[index] = sparse[index] = 0x001f;
        }
        assert(pxa_surface_blend_rgb565_a8(
                   full, 4, 3, 4, colors, alpha, 5, 2, 6, 6,
                   -1, 1, 255) == 4);
        for (size_t index = 0; index < count; ++index) {
            const pxa_surface_alpha_span_t span = spans[index];
            (void)pxa_surface_blend_rgb565_a8(
                sparse, 4, 3, 4,
                colors + (size_t)span.y * 6 + span.x,
                alpha + (size_t)span.y * 6 + span.x,
                span.width, 1, span.width, span.width,
                -1 + span.x, 1 + span.y, 255);
        }
        assert(memcmp(full, sparse, sizeof(full)) == 0);
    }
}

int main(void) {
    assert(pxa_surface_integer_scale(296, 240, 296, 240) == 1);
    assert(pxa_surface_integer_scale(148, 120, 296, 240) == 2);
    assert(pxa_surface_integer_scale(74, 60, 296, 240) == 4);
    assert(pxa_surface_integer_scale(147, 120, 296, 240) == 0);
    assert(pxa_surface_integer_scale(0, 120, 296, 240) == 0);
    assert(pxa_surface_fit_scale(296, 240, 296, 240) == 1);
    assert(pxa_surface_fit_scale(148, 120, 296, 240) == 2);
    assert(pxa_surface_fit_scale(148, 120, 412, 412) == 2);
    assert(pxa_surface_fit_scale(296, 240, 412, 412) == 1);
    assert(pxa_surface_fit_scale(206, 206, 412, 412) == 2);
    assert(pxa_surface_fit_scale(103, 103, 412, 412) == 4);
    assert(pxa_surface_fit_scale(207, 206, 412, 412) == 1);
    assert(pxa_surface_fit_scale(147, 120, 296, 240) == 2);
    assert(pxa_surface_fit_scale(413, 412, 412, 412) == 0);
    assert(pxa_surface_fit_scale(0, 120, 412, 412) == 0);
    assert(pxa_surface_fit_scale(148, 120, 0, 412) == 0);
    check_scale(1, 0);
    check_scale(1, 1);
    check_scale(2, 0);
    check_scale(2, 1);
    check_scale(4, 0);
    check_scale(4, 1);
    check_upscale(1);
    check_upscale(2);
    check_upscale(4);
    check_capture_round_trip();
    check_alpha_blend();
    check_alpha_spans();
    assert(!pxa_surface_transform_rgb565_270_columns(
        NULL, NULL, 4, 3, 0, 4, 2, false));
    assert(!pxa_surface_upscale_rgb565_nearest(
        NULL, NULL, 4, 3, 4, 8, 2));
    assert(!pxa_surface_capture_rgb565_270_to_logical(
        NULL, NULL, 4, 3, 3, 4, true));
    return 0;
}
