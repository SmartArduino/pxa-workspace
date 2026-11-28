// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <driver/ppa.h>

namespace pxa::board {
// The caller owns both buffers until the blocking SRM transaction completes.
// Stride is in pixels. The output occupies a complete, cache-aligned buffer.
inline esp_err_t RotatePpaRgb565(ppa_client_handle_t client, const uint16_t* source,
                                 uint32_t stride, uint32_t width, uint32_t height,
                                 uint16_t* output, ppa_srm_rotation_angle_t angle,
                                 bool swap_bytes) {
    const bool quarter_turn = angle == PPA_SRM_ROTATION_ANGLE_90 || angle == PPA_SRM_ROTATION_ANGLE_270;
    ppa_srm_oper_config_t operation{};
    operation.in.buffer = source;
    operation.in.pic_w = stride;
    operation.in.block_w = width;
    operation.in.pic_h = operation.in.block_h = height;
    operation.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    operation.out.buffer = output;
    operation.out.buffer_size = width * height * sizeof(uint16_t);
    operation.out.pic_w = quarter_turn ? height : width;
    operation.out.pic_h = quarter_turn ? width : height;
    operation.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    operation.rotation_angle = angle;
    operation.scale_x = operation.scale_y = 1.0f;
    operation.byte_swap = swap_bytes;
    operation.alpha_update_mode = PPA_ALPHA_NO_CHANGE;
    operation.mode = PPA_TRANS_MODE_BLOCKING;
    return ppa_do_scale_rotate_mirror(client, &operation);
}
}
