// SPDX-License-Identifier: Apache-2.0
#ifndef PXA_EXT_VISION_FRAME_H
#define PXA_EXT_VISION_FRAME_H
#include "peripherals_protocol.h"
#include <stddef.h>
#include <string.h>
typedef struct {
    uint16_t format;
    uint32_t size, sequence;
    uint64_t monotonic_us;
} pxa_vision_frame_info_t;
static inline void pxa_vision_put_le(uint8_t *p, uint64_t value, unsigned n) {
    for (unsigned i = 0; i < n; ++i) p[i] = (uint8_t)(value >> (i * 8));
}
static inline uint64_t pxa_vision_get_le(const uint8_t *p, unsigned n) {
    uint64_t result = 0;
    for (unsigned i = 0; i < n; ++i) result |= (uint64_t)p[i] << (i * 8);
    return result;
}
static inline int pxa_vision_valid_size(uint16_t format, uint32_t size) {
    return size && ((format == PXA_VISION_JPEG && size <= PXA_VISION_MAX_JPEG) ||
        (format == PXA_VISION_JSON && size <= PXA_VISION_MAX_JSON));
}
static inline int pxa_vision_encode(uint8_t header[PXA_VISION_HEADER_BYTES],
    const pxa_vision_frame_info_t *info) {
    if (!pxa_vision_valid_size(info->format, info->size)) return 0;
    memcpy(header, "PXVF", 4);
    pxa_vision_put_le(header + 4, 1, 2);
    pxa_vision_put_le(header + 6, info->format, 2);
    pxa_vision_put_le(header + 8, info->size, 4);
    pxa_vision_put_le(header + 12, info->sequence, 4);
    pxa_vision_put_le(header + 16, info->monotonic_us, 8);
    return 1;
}
static inline int pxa_vision_decode(const uint8_t header[PXA_VISION_HEADER_BYTES],
    pxa_vision_frame_info_t *info) {
    if (memcmp(header, "PXVF", 4) || pxa_vision_get_le(header + 4, 2) != 1) return 0;
    pxa_vision_frame_info_t decoded;
    decoded.format = (uint16_t)pxa_vision_get_le(header + 6, 2);
    decoded.size = (uint32_t)pxa_vision_get_le(header + 8, 4);
    decoded.sequence = (uint32_t)pxa_vision_get_le(header + 12, 4);
    decoded.monotonic_us = pxa_vision_get_le(header + 16, 8);
    if (!pxa_vision_valid_size(decoded.format, decoded.size)) return 0;
    *info = decoded;
    return 1;
}
#endif
