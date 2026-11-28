// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <algorithm>
#include <cstring>
#include <new>
#include "driver/ppa.h"
#include "esp_openh264_dec.h"
#include "pxa/pxa_esp_surface.h"
#include "pxa_video_rgb565.hpp"
#if PXA_GAMEKIT_BOX420_360
#include "pxa_video_box420.hpp"
#endif
extern "C" bool pxa_video_pack_ppa420(const uint8_t *, const uint8_t *, const uint8_t *,
    unsigned, unsigned, unsigned, unsigned, uint8_t *, unsigned);

namespace pxa_video::native_surface {
struct Rect { unsigned x, y, w, h; };
// The aligned packed pixels follow the header. The Surface owns both until
// this slot is no longer readable. No address points into the decoder.
struct alignas(128) Picture {
    uint16_t width, height, pitch, scale_step, logical_width, logical_height;
    Rect region;
};
static_assert(sizeof(Picture) == 128);

inline ppa_client_handle_t scaler() {
    // One Host client serves all display leases, including a paused picture
    // that outlives the decoder. The renderer holds only one Surface lease.
    static const ppa_client_handle_t client = [] {
        ppa_client_config_t config{};
        config.oper_type = PPA_OPERATION_SRM;
        config.max_pending_trans_num = 1;
        config.data_burst_length = PPA_DATA_BURST_LENGTH_128;
        ppa_client_handle_t value = nullptr;
        return ppa_register_client(&config, &value) == ESP_OK ? value : nullptr;
    }();
    return client;
}

inline Rect rotate_rect(const Rect &r, unsigned width, unsigned height, uint8_t rotation) {
    switch (rotation) {
        case 1: return {height-r.y-r.h, r.x, r.h, r.w};
        case 2: return {width-r.x-r.w, height-r.y-r.h, r.w, r.h};
        case 3: return {r.y, width-r.x-r.w, r.h, r.w};
        default: return r;
    }
}

inline void clear_borders(const pxa_esp_surface_native_target_t &target, const Rect &r) {
    // PPA invalidates output cache lines before DMA. Paint the bars afterwards
    // so the engine cannot discard border pixels that share a cache line.
    const unsigned stride = target.stride_bytes / sizeof(uint16_t);
    uint16_t *row = target.pixels;
    for (unsigned y=0; y<r.y; ++y, row+=stride) std::fill_n(row, target.width, 0);
    const unsigned right = r.x + r.w;
    if (r.x || right < target.width) {
        for (unsigned y=0; y<r.h; ++y, row+=stride) {
            std::fill_n(row, r.x, 0);
            std::fill_n(row+right, target.width-right, 0);
        }
    } else row += size_t(r.h)*stride;
    for (unsigned y=r.y+r.h; y<target.height; ++y, row+=stride) std::fill_n(row, target.width, 0);
}

inline void draw_logical_cpu(const Picture &p, const uint8_t *packed,
                             const pxa_esp_surface_native_target_t &target) {
    const size_t input_stride = size_t(p.pitch)*3/2;
    const size_t output_stride = target.stride_bytes/sizeof(uint16_t);
    const auto &r = p.region;
    for (unsigned y=0; y<r.h; ++y) {
        const unsigned sy = y*p.height/r.h;
        const auto *luma = packed + size_t(sy)*input_stride;
        const auto *chroma = packed + size_t(sy & ~1u)*input_stride;
        auto *dst = target.pixels + size_t(r.y+y)*output_stride + r.x;
        for (unsigned x=0; x<r.w; ++x) {
            const unsigned sx = x*p.width/r.w;
            const unsigned pair = (sx/2)*3;
            dst[x] = rgb565_detail::sample(luma[pair+1+(sx&1)],
                                          chroma[pair], chroma[input_stride+pair]);
        }
    }
}

inline bool draw(const void *payload, const pxa_esp_surface_native_target_t *target) {
    const auto &p = *static_cast<const Picture *>(payload);
    const bool quarter_turn = target->rotation & 1;
    const unsigned expected_width = quarter_turn ? p.logical_height : p.logical_width;
    const unsigned expected_height = quarter_turn ? p.logical_width : p.logical_height;
    if (!target->pixels || target->rotation > 3 || target->width != expected_width ||
        target->height != expected_height || (target->stride_bytes & 1) ||
        target->stride_bytes < expected_width*2 ||
        target->size < size_t(target->stride_bytes)*(expected_height-1)+expected_width*2) return false;
    const auto r = rotate_rect(p.region,p.logical_width,p.logical_height,target->rotation);
    const auto *packed = reinterpret_cast<const uint8_t *>(&p+1);
    ppa_srm_oper_config_t op{};
    op.in.buffer = packed;
    op.in.pic_w = p.pitch;
    op.in.block_w = p.width;
    op.in.pic_h = op.in.block_h = p.height;
    op.in.srm_cm = PPA_SRM_COLOR_MODE_YUV420;
    op.in.yuv_range = PPA_COLOR_RANGE_LIMIT;
    op.in.yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601;
    op.out.buffer = target->pixels;
    op.out.buffer_size = target->size;
    op.out.pic_w = target->stride_bytes/2;
    op.out.pic_h = target->height;
    op.out.block_offset_x = r.x;
    op.out.block_offset_y = r.y;
    op.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    constexpr ppa_srm_rotation_angle_t angles[] = {PPA_SRM_ROTATION_ANGLE_0,
        PPA_SRM_ROTATION_ANGLE_270,PPA_SRM_ROTATION_ANGLE_180,PPA_SRM_ROTATION_ANGLE_90};
    op.rotation_angle = angles[target->rotation];
    op.scale_x = op.scale_y = p.scale_step / 16.0f;
    op.mode = PPA_TRANS_MODE_BLOCKING;
    if (!scaler() || ppa_do_scale_rotate_mirror(scaler(), &op) != ESP_OK) {
        if (target->rotation) return false;
        draw_logical_cpu(p, packed, *target);
    }
    clear_borders(*target,r);
    return true;
}

inline bool stage(const esp_openh264_dec_frame_t &frame, uint8_t *logical_pixels,
                  unsigned logical_width, unsigned logical_height, const Rect &region, unsigned step) {
    if (!step || (frame.width | frame.height) & 1 || !scaler()) return false;
    unsigned width = frame.width;
    unsigned height = frame.height;
#if PXA_GAMEKIT_BOX420_360
    // Both the software raster fallback and the PPA use this same picture/ROI.
    const bool box = width == 640 && height == 360 && step == 8 &&
        region.w == 320 && region.h == 180 && logical_width >= region.w &&
        logical_height >= region.h && region.x <= logical_width - region.w &&
        region.y <= logical_height - region.h;
    if (box) {
        width /= 2;
        height /= 2;
        step *= 2;
    }
#endif
    const unsigned pitch = (width + 31u) & ~31u;
    const size_t bytes = size_t(pitch) * height * 3 / 2;
    void *payload = nullptr;
    if (!pxa_esp_surface_stage_native(logical_pixels, sizeof(Picture)+bytes, draw, &payload)) return false;
    auto *p = new(payload) Picture{static_cast<uint16_t>(width),static_cast<uint16_t>(height),static_cast<uint16_t>(pitch),
        static_cast<uint16_t>(step),static_cast<uint16_t>(logical_width),
        static_cast<uint16_t>(logical_height),region};
    auto *packed = reinterpret_cast<uint8_t *>(p+1);
#if PXA_GAMEKIT_BOX420_360
    if (box) {
        pack_box420(frame.planes, frame.stride, frame.width, frame.height, packed, pitch);
        return true;
    }
#endif
    if (pxa_video_pack_ppa420(frame.planes[0],frame.planes[1],frame.planes[2],
        frame.width,frame.height,frame.stride[0],frame.stride[1],packed,pitch)) return true;
    pxa_esp_surface_cancel_native(logical_pixels);
    return false;
}
}
