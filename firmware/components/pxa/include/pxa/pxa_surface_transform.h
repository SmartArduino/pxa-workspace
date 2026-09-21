#ifndef PXA_SURFACE_TRANSFORM_H
#define PXA_SURFACE_TRANSFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline uint8_t pxa_surface_integer_scale(uint16_t source_width,
                                                uint16_t source_height,
                                                uint16_t target_width,
                                                uint16_t target_height) {
    uint8_t scale;
    if (source_width == 0 || source_height == 0 || target_width == 0 ||
        target_height == 0)
        return 0;
    for (scale = 1; scale <= 4; scale = (uint8_t)(scale * 2)) {
        if ((uint32_t)source_width * scale == target_width &&
            (uint32_t)source_height * scale == target_height)
            return scale;
    }
    return 0;
}

/* Largest 1x/2x/4x factor that keeps the whole Surface inside the target.
 * Boards use this to present a Surface that is smaller than the panel at the
 * biggest exact upscale that fits; the Guest repeats the same computation to
 * map panel input coordinates back into Surface pixels. Returns 0 when either
 * size is empty or even 1x does not fit, otherwise the best factor. */
static inline uint8_t pxa_surface_fit_scale(uint16_t source_width,
                                            uint16_t source_height,
                                            uint16_t target_width,
                                            uint16_t target_height) {
    uint8_t scale;
    uint8_t best = 0;
    if (source_width == 0 || source_height == 0 || target_width == 0 ||
        target_height == 0)
        return 0;
    for (scale = 1; scale <= 4; scale = (uint8_t)(scale * 2)) {
        if ((uint32_t)source_width * scale <= target_width &&
            (uint32_t)source_height * scale <= target_height)
            best = scale;
    }
    return best;
}

static inline bool pxa_surface_upscale_rgb565_nearest(
    const uint16_t *source, uint16_t *output, uint16_t source_width,
    uint16_t source_height, uint32_t source_stride_pixels,
    uint32_t output_stride_pixels, uint8_t scale) {
    uint16_t source_y;
    if (source == NULL || output == NULL || source_width == 0 ||
        source_height == 0 || source_stride_pixels < source_width ||
        output_stride_pixels < (uint32_t)source_width * scale ||
        (scale != 1 && scale != 2 && scale != 4))
        return false;
    for (source_y = 0; source_y < source_height; ++source_y) {
        const uint16_t *source_row = source +
            (uint32_t)source_y * source_stride_pixels;
        uint8_t duplicate_y;
        for (duplicate_y = 0; duplicate_y < scale; ++duplicate_y) {
            uint16_t *output_row = output +
                ((uint32_t)source_y * scale + duplicate_y) *
                    output_stride_pixels;
            uint16_t source_x;
            for (source_x = 0; source_x < source_width; ++source_x) {
                uint8_t duplicate_x;
                for (duplicate_x = 0; duplicate_x < scale; ++duplicate_x)
                    *output_row++ = source_row[source_x];
            }
        }
    }
    return true;
}

/* Blends a logical RGB565+A8 plane over an RGB565 destination. The plane may
 * be clipped by any target edge. Transparent pixels are skipped so sparse HUD
 * overlays do not read or write the destination framebuffer unnecessarily.
 * Returns the number of destination pixels that were changed. */
static inline uint32_t pxa_surface_blend_rgb565_a8(
    uint16_t *destination, uint16_t destination_width,
    uint16_t destination_height, uint32_t destination_stride_pixels,
    const uint16_t *foreground, const uint8_t *alpha,
    uint16_t foreground_width, uint16_t foreground_height,
    uint32_t foreground_stride_pixels, uint32_t alpha_stride_bytes,
    int32_t origin_x, int32_t origin_y, uint8_t opacity) {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
    uint32_t blended = 0;
    int32_t y;
    if (destination == NULL || foreground == NULL || alpha == NULL ||
        destination_width == 0 || destination_height == 0 ||
        foreground_width == 0 || foreground_height == 0 || opacity == 0 ||
        destination_stride_pixels < destination_width ||
        foreground_stride_pixels < foreground_width ||
        alpha_stride_bytes < foreground_width)
        return 0;
    left = origin_x < 0 ? 0 : origin_x;
    top = origin_y < 0 ? 0 : origin_y;
    right = origin_x + (int32_t)foreground_width;
    bottom = origin_y + (int32_t)foreground_height;
    if (right > destination_width) right = destination_width;
    if (bottom > destination_height) bottom = destination_height;
    if (left >= right || top >= bottom) return 0;
    for (y = top; y < bottom; ++y) {
        const uint32_t source_y = (uint32_t)(y - origin_y);
        const uint32_t source_x = (uint32_t)(left - origin_x);
        const uint16_t *source_row = foreground +
            source_y * foreground_stride_pixels + source_x;
        const uint8_t *alpha_row = alpha +
            source_y * alpha_stride_bytes + source_x;
        uint16_t *destination_row = destination +
            (uint32_t)y * destination_stride_pixels + (uint32_t)left;
        int32_t x = left;
        while (x < right) {
            if (opacity == 255 && right - x >= 4 &&
                (alpha_row[0] & alpha_row[1] &
                 alpha_row[2] & alpha_row[3]) == 255) {
                memcpy(destination_row, source_row,
                       4 * sizeof(*destination_row));
                source_row += 4;
                alpha_row += 4;
                destination_row += 4;
                blended += 4;
                x += 4;
                continue;
            }
            if (right - x >= 4 &&
                (alpha_row[0] | alpha_row[1] |
                 alpha_row[2] | alpha_row[3]) == 0) {
                source_row += 4;
                alpha_row += 4;
                destination_row += 4;
                x += 4;
                continue;
            }
            uint8_t effective_alpha = *alpha_row++;
            uint16_t source;
            uint16_t target;
            uint16_t inverse;
            uint16_t red;
            uint16_t green;
            uint16_t blue;
            if (effective_alpha == 0) {
                ++source_row;
                ++destination_row;
                ++x;
                continue;
            }
            if (opacity != 255) {
                effective_alpha = (uint8_t)(
                    ((uint16_t)effective_alpha * opacity + 127u) / 255u);
                if (effective_alpha == 0) {
                    ++source_row;
                    ++destination_row;
                    ++x;
                    continue;
                }
            }
            source = *source_row++;
            ++blended;
            if (effective_alpha == 255) {
                *destination_row++ = source;
                ++x;
                continue;
            }
            target = *destination_row;
            inverse = (uint16_t)(255u - effective_alpha);
            red = (uint16_t)(((source >> 11) * effective_alpha +
                              (target >> 11) * inverse + 128u) >> 8);
            green = (uint16_t)((((source >> 5) & 0x3fu) * effective_alpha +
                                ((target >> 5) & 0x3fu) * inverse + 128u) >>
                               8);
            blue = (uint16_t)(((source & 0x1fu) * effective_alpha +
                               (target & 0x1fu) * inverse + 128u) >> 8);
            *destination_row++ =
                (uint16_t)((red << 11) | (green << 5) | blue);
            ++x;
        }
    }
    return blended;
}

typedef struct {
    uint16_t x;
    uint16_t y;
    uint16_t width;
} pxa_surface_alpha_span_t;

/* The alpha plane changes less often than the world. Cache its nontransparent
 * horizontal runs so each world frame only visits visible UI pixels. */
static inline size_t pxa_surface_alpha_spans(
    const uint8_t *alpha, uint16_t width, uint16_t height,
    uint32_t stride_bytes, pxa_surface_alpha_span_t *spans,
    size_t capacity) {
    size_t count = 0;
    if (alpha == NULL || spans == NULL || stride_bytes < width)
        return SIZE_MAX;
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t *row = alpha + (size_t)y * stride_bytes;
        uint32_t x = 0;
        while (x < width) {
            while (x < width && row[x] == 0) ++x;
            if (x == width) break;
            const uint32_t first = x;
            while (x < width && row[x] != 0) ++x;
            if (count == capacity) return SIZE_MAX;
            spans[count].x = (uint16_t)first;
            spans[count].y = (uint16_t)y;
            spans[count].width = (uint16_t)(x - first);
            ++count;
        }
    }
    return count;
}

/* Fused nearest-neighbor upscale, 270-degree logical-to-panel rotation and
 * optional RGB565 byte swap. Column ranges allow two workers to write
 * disjoint native rows without synchronization. */
static inline bool pxa_surface_transform_rgb565_270_columns(
    const uint16_t *source, uint16_t *output, uint16_t source_width,
    uint16_t source_height, uint16_t first_column, uint16_t last_column,
    uint8_t scale, bool source_byte_swapped) {
    uint16_t source_x;
    uint16_t output_row;
    if (source == NULL || output == NULL || source_width == 0 ||
        source_height == 0 || first_column > last_column ||
        last_column > source_width ||
        (scale != 1 && scale != 2 && scale != 4) ||
        source_height > UINT16_MAX / scale)
        return false;
    output_row = (uint16_t)(source_height * scale);
    for (source_x = first_column; source_x < last_column; ++source_x) {
        uint16_t *destinations[4] = {NULL, NULL, NULL, NULL};
        int32_t source_y;
        uint8_t duplicate_x;
        for (duplicate_x = 0; duplicate_x < scale; ++duplicate_x) {
            destinations[duplicate_x] =
                output + ((uint32_t)source_x * scale + duplicate_x) *
                             output_row;
        }
        for (source_y = (int32_t)source_height - 1; source_y >= 0;
             --source_y) {
            uint16_t value =
                source[(uint32_t)source_y * source_width + source_x];
            uint8_t duplicate_y;
            if (!source_byte_swapped)
                value = (uint16_t)((value << 8) | (value >> 8));
            for (duplicate_x = 0; duplicate_x < scale; ++duplicate_x) {
                for (duplicate_y = 0; duplicate_y < scale; ++duplicate_y)
                    *destinations[duplicate_x]++ = value;
            }
        }
    }
    return true;
}

/* Restores a completed native panel buffer produced by the 270-degree
 * transform to logical row-major RGB565. The panel buffer normally contains
 * wire-order (byte-swapped) pixels. */
static inline bool pxa_surface_capture_rgb565_270_to_logical(
    const uint16_t *physical, uint16_t *logical, uint16_t logical_width,
    uint16_t logical_height, uint32_t physical_stride_pixels,
    uint32_t logical_stride_pixels, bool physical_byte_swapped) {
    uint16_t y;
    if (physical == NULL || logical == NULL || logical_width == 0 ||
        logical_height == 0 || physical_stride_pixels < logical_height ||
        logical_stride_pixels < logical_width)
        return false;
    for (y = 0; y < logical_height; ++y) {
        uint16_t x;
        uint16_t *logical_row = logical + (uint32_t)y * logical_stride_pixels;
        for (x = 0; x < logical_width; ++x) {
            uint16_t value = physical[
                (uint32_t)x * physical_stride_pixels + logical_height - 1u - y];
            if (physical_byte_swapped)
                value = (uint16_t)((value << 8) | (value >> 8));
            logical_row[x] = value;
        }
    }
    return true;
}

#ifdef __cplusplus
}
#endif

#endif
