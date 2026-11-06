// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>
#include <stdbool.h>
typedef struct {
    uint8_t jpeg_mode, in_bytes_per_pixel, fb_bytes_per_pixel;
    bool psram_mode;
    uint16_t width, height;
    uint32_t dma_bytes_per_item, dma_node_buffer_size, dma_half_buffer_size;
    uint32_t dma_buffer_size, dma_half_buffer_cnt;
} cam_obj_t;
