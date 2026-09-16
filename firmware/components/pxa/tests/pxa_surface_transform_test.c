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

int main(void) {
    assert(pxa_surface_integer_scale(296, 240, 296, 240) == 1);
    assert(pxa_surface_integer_scale(148, 120, 296, 240) == 2);
    assert(pxa_surface_integer_scale(74, 60, 296, 240) == 4);
    assert(pxa_surface_integer_scale(147, 120, 296, 240) == 0);
    assert(pxa_surface_integer_scale(0, 120, 296, 240) == 0);
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
    assert(!pxa_surface_transform_rgb565_270_columns(
        NULL, NULL, 4, 3, 0, 4, 2, false));
    assert(!pxa_surface_upscale_rgb565_nearest(
        NULL, NULL, 4, 3, 4, 8, 2));
    assert(!pxa_surface_capture_rgb565_270_to_logical(
        NULL, NULL, 4, 3, 3, 4, true));
    return 0;
}
