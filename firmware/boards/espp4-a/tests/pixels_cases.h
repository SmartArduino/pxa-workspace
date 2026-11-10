// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "espp4_a_pixels.h"
#include <algorithm>
#include <cstring>

inline bool TestEspp4Pixels() {
    alignas(16) uint16_t source[160], actual[640], expected[640];
    for (unsigned i = 0; i < 160; ++i) source[i] = uint16_t((i * 1709u) ^ (i << 9) ^ 0x527a);
    constexpr unsigned counts[] = {0,1,7,8,9,15,16,17,31,32,33,63,64,127,128};
    for (unsigned offset = 0; offset < 8; ++offset) for (unsigned count : counts) {
        std::fill_n(actual,640,0xa35c);std::fill_n(expected,640,0xa35c);
        std::fill_n(expected + offset,count,0x0f71);
        espp4_a_fill_rgb565(actual + offset,count,0x0f71);
        if (std::memcmp(actual,expected,sizeof(actual))) return false;
    }
    for (unsigned in = 0; in < 8; ++in) for (unsigned out = 0; out < 8; ++out)
        for (unsigned scale : {2u,4u}) for (unsigned count : counts) {
            std::fill_n(actual,640,0xa35c);std::fill_n(expected,640,0xa35c);
            for (unsigned i = 0; i < count; ++i)
                std::fill_n(expected + out + i * scale,scale,source[in+i]);
            espp4_a_expand_rgb565(actual+out,source+in,count,scale);
            if (std::memcmp(actual,expected,sizeof(actual))) return false;
        }
    return true;
}
