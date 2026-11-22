// SPDX-License-Identifier: Apache-2.0
#include "pxa_video_rgb565.hpp"
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif
#if defined(CONFIG_IDF_TARGET_ESP32P4) && CONFIG_IDF_TARGET_ESP32P4 && !CONFIG_ESP32P4_SELECTS_REV_LESS_V3
extern "C" void pxa_video_pie_pack_row(const uint8_t *,const uint8_t *,uint8_t *,unsigned);
#endif
extern "C" void pxa_video_convert_rgb565(const uint8_t *y,const uint8_t *u,const uint8_t *v,
    unsigned width,unsigned height,unsigned y_stride,unsigned chroma_stride,uint8_t *out) {
    pxa_video::yuv420_rgb565(y,u,v,width,height,y_stride,chroma_stride,out);
}
extern "C" bool pxa_video_pack_ppa420(const uint8_t *y,const uint8_t *u,const uint8_t *v,
    unsigned width,unsigned height,unsigned y_stride,unsigned chroma_stride,uint8_t *out) {
#if defined(CONFIG_IDF_TARGET_ESP32P4) && CONFIG_IDF_TARGET_ESP32P4 && !CONFIG_ESP32P4_SELECTS_REV_LESS_V3
    if(width && !(width&31) && height && !(height&1) &&
        !((uintptr_t(y)|uintptr_t(u)|uintptr_t(v)|uintptr_t(out)|y_stride|chroma_stride)&15)) {
        for(unsigned row=0;row<height;++row)
            pxa_video_pie_pack_row(y+size_t(row)*y_stride,(row&1?v:u)+size_t(row/2)*chroma_stride,
                out+size_t(row)*width*3/2,width/32);
        return true;
    }
#endif
    return pxa_video::pack_ppa420(y,u,v,width,height,y_stride,chroma_stride,out);
}
