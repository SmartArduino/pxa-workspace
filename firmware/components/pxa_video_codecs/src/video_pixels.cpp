// SPDX-License-Identifier: Apache-2.0
#include "pxa_video_rgb565.hpp"
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif
#if CONFIG_IDF_TARGET_ESP32S31
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif
#if CONFIG_IDF_TARGET_ESP32S31 || (CONFIG_IDF_TARGET_ESP32P4 && !CONFIG_ESP32P4_SELECTS_REV_LESS_V3)
extern "C" void pxa_video_pie_half444_row(const uint8_t *, const uint8_t *, const uint8_t *, uint8_t *, unsigned);
extern "C" void pxa_video_pie_pack_row(const uint8_t *,const uint8_t *,uint8_t *,unsigned);
#endif
namespace {
[[maybe_unused]] bool can_pack_vectors() {
#if CONFIG_IDF_TARGET_ESP32S31
    return xPortGetCoreID() == 1 && xTaskGetCoreID(nullptr) == 1;
#elif CONFIG_IDF_TARGET_ESP32P4 && !CONFIG_ESP32P4_SELECTS_REV_LESS_V3
    return true;
#else
    return false;
#endif
}
}
extern "C" void pxa_video_pack_half444(const uint8_t *y, const uint8_t *u, const uint8_t *v,
    unsigned width, unsigned height, unsigned ys, unsigned cs, uint8_t *out, unsigned pitch) {
    // Exactly one luma sample and one independent chroma pair per output pixel.
    unsigned vector_width = 0;
#if CONFIG_IDF_TARGET_ESP32S31 || (CONFIG_IDF_TARGET_ESP32P4 && !CONFIG_ESP32P4_SELECTS_REV_LESS_V3)
    if (can_pack_vectors() && !((uintptr_t(y)|uintptr_t(u)|uintptr_t(v)|uintptr_t(out)|ys|cs|pitch)&15))
        vector_width = width & ~15u;
#endif
    for (unsigned row=0; row<height; ++row) {
#if CONFIG_IDF_TARGET_ESP32S31 || (CONFIG_IDF_TARGET_ESP32P4 && !CONFIG_ESP32P4_SELECTS_REV_LESS_V3)
        if (vector_width) pxa_video_pie_half444_row(y,u,v,out,vector_width/16);
#endif
        for (unsigned x=vector_width; x<width; ++x) {
            out[x*3]=y[x*2]; out[x*3+1]=u[x]; out[x*3+2]=v[x];
        }
        y+=2*ys; u+=cs; v+=cs; out+=3*pitch;
    }
}
extern "C" void pxa_video_convert_rgb565(const uint8_t *y,const uint8_t *u,const uint8_t *v,
    unsigned width,unsigned height,unsigned y_stride,unsigned chroma_stride,uint8_t *out) {
    pxa_video::yuv420_rgb565(y,u,v,width,height,y_stride,chroma_stride,out);
}
extern "C" bool pxa_video_pack_ppa420(const uint8_t *y,const uint8_t *u,const uint8_t *v,
    unsigned width, unsigned height, unsigned y_stride, unsigned chroma_stride, uint8_t *out, unsigned pitch) {
#if CONFIG_IDF_TARGET_ESP32S31 || (CONFIG_IDF_TARGET_ESP32P4 && !CONFIG_ESP32P4_SELECTS_REV_LESS_V3)
    const unsigned vector_width = width & ~31u;
    if (can_pack_vectors() && vector_width && pitch >= width && !(pitch & 31) && height && !((width | height) & 1) &&
        y_stride >= width && chroma_stride >= width / 2 &&
        !((uintptr_t(y) | uintptr_t(u) | uintptr_t(v) | uintptr_t(out) | y_stride | chroma_stride) & 15)) {
        for (unsigned row = 0; row < height; ++row) {
            const auto *luma = y + size_t(row) * y_stride;
            const auto *chroma = (row & 1 ? v : u) + size_t(row / 2) * chroma_stride;
            auto *dest = out + size_t(row) * pitch * 3 / 2;
            pxa_video_pie_pack_row(luma, chroma, dest, vector_width / 32);
            if (width != vector_width)
                pxa_video::pack_ppa420_row(luma + vector_width, chroma + vector_width / 2,
                                           width - vector_width, dest + vector_width * 3 / 2);
        }
        return true;
    }
#endif
    return pxa_video::pack_ppa420(y, u, v, width, height, y_stride, chroma_stride, out, pitch);
}

#if PXA_GAMEKIT_BOX420_360
#include "pxa_video_box420.hpp"
#if CONFIG_IDF_TARGET_ESP32S31
extern "C" void pxa_video_pie_box420_row(const uint8_t *, const uint8_t *,
    const uint8_t *, const uint8_t *, uint8_t *, unsigned);
#endif

namespace pxa_video {
namespace {
uint8_t box_sample(const uint8_t *top, const uint8_t *bottom, unsigned column) {
    const unsigned a = (top[column] + top[column + 1] + 1u) / 2;
    const unsigned b = (bottom[column] + bottom[column + 1] + 1u) / 2;
    return static_cast<uint8_t>((a + b + 1) / 2);
}
}

void pack_box420(const uint8_t *const planes[3], const int32_t strides[3],
                 unsigned width, unsigned height, uint8_t *output, unsigned pitch) {
    unsigned vector_width = 0;
#if CONFIG_IDF_TARGET_ESP32S31
    const uintptr_t alignment = uintptr_t(planes[0]) | uintptr_t(planes[1]) |
        uintptr_t(planes[2]) | uintptr_t(output) | strides[0] | strides[1] | strides[2];
    if (!(alignment & 15u) && !(pitch & 31u) && can_pack_vectors()) {
        vector_width = width & ~63u;
    }
#endif
    for (unsigned row = 0; row < height / 2; ++row) {
        const unsigned plane = (row & 1u) ? 2 : 1;
        const auto *y = planes[0] + size_t(row * 2) * strides[0];
        // Adjacent output rows carry U and V from the same source chroma pair.
        const auto *c = planes[plane] + size_t(row & ~1u) * strides[plane];
#if CONFIG_IDF_TARGET_ESP32S31
        if (vector_width) {
            pxa_video_pie_box420_row(y, y + strides[0], c, c + strides[plane],
                                     output, vector_width / 64);
        }
#endif
        auto *dst = output + size_t(vector_width) * 3 / 4;
        for (unsigned x = vector_width; x < width; x += 4) {
            *dst++ = box_sample(c, c + strides[plane], x / 2);
            *dst++ = box_sample(y, y + strides[0], x);
            *dst++ = box_sample(y, y + strides[0], x + 2);
        }
        output += size_t(pitch) * 3 / 2;
    }
}
}
#endif
