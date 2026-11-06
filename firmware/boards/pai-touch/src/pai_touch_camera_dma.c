// SPDX-License-Identifier: Apache-2.0
#include "ll_cam.h"

bool __real_ll_cam_dma_sizes(cam_obj_t *camera);

// The generic driver reserves at least 8 KiB of internal DMA memory. This
// board uses two smaller, whole-row nodes for RGB565 and YUYV. Each EOF copies one
// node into the PSRAM framebuffer before the DMA ring reuses that node.
bool __wrap_ll_cam_dma_sizes(cam_obj_t *camera)
{
    if (camera->jpeg_mode || camera->psram_mode ||
        camera->in_bytes_per_pixel != 2 || camera->fb_bytes_per_pixel != 2) {
        return __real_ll_cam_dma_sizes(camera);
    }
    const uint32_t half_limit = 2048;
    const uint32_t row_bytes = camera->width * 2;
    if (!row_bytes || row_bytes > half_limit || !camera->height) return false;
    uint32_t rows = half_limit / row_bytes;
    if (rows > camera->height) rows = camera->height;
    while (camera->height % rows) --rows;
    const uint32_t node_bytes = rows * row_bytes;
    camera->dma_bytes_per_item = 1;
    camera->dma_node_buffer_size = node_bytes;
    camera->dma_half_buffer_size = node_bytes;
    camera->dma_buffer_size = node_bytes * 2;
    camera->dma_half_buffer_cnt = 2;
    return true;
}
