#ifndef PXA_SURFACE_TRANSFORM_H
#define PXA_SURFACE_TRANSFORM_H

#include <stdbool.h>
#include <stdint.h>

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
