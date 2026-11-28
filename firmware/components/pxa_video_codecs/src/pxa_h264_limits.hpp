// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "pxa/status.h"
#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <iterator>
namespace pxa_video {
inline constexpr uint8_t h264_max_level = 51;
/* SPS bounds are checked before the decoder can allocate its reference pictures.
 * Only progressive 8-bit 4:2:0 Baseline/Main/High is advertised. */
struct Bits {
    uint8_t bytes[1024]{};
    size_t count = 0, bit = 0;
    bool bad = false;
    uint32_t get(unsigned n) {
        if (n > 32 || n > count*8 - std::min(bit, count*8)) { bad = true; return 0; }
        uint32_t v = 0; while (n--) { v = (v<<1) | ((bytes[bit/8] >> (7-bit%8)) & 1); ++bit; } return v;
    }
    uint32_t ue() {
        unsigned n = 0; while (!bad && !get(1)) if (++n > 24) { bad = true; break; }
        return bad ? 0 : ((1u<<n)-1 + get(n));
    }
    int32_t se() { uint32_t n = ue(); return n&1 ? int32_t((n+1)/2) : -int32_t(n/2); }
};
inline pxa_status_t check_sps(const uint8_t *data, size_t size, uint16_t max_width, uint16_t max_height) {
    if (size < 4 || size > 1024) return PXA_STATUS_PROTOCOL_ERROR;
    Bits b;
    unsigned zeros = 0;
    for (size_t i = 1; i < size; ++i) {
        if (zeros == 2 && data[i] == 3) { zeros = 0; continue; }
        b.bytes[b.count++] = data[i]; zeros = data[i] ? 0 : zeros+1;
    }
    unsigned profile = b.get(8); b.get(8); unsigned level = b.get(8); b.ue();
    if (profile != 66 && profile != 77 && profile != 100) return PXA_STATUS_UNSUPPORTED;
    // A small stream may declare a high level. Level alone is not its actual
    // resource requirement; keep checking coded dimensions and reference bounds.
    constexpr uint8_t levels[]{9,10,11,12,13,20,21,22,30,31,32,40,41,42,50,51};
    if (std::find(std::begin(levels),std::end(levels),level)==std::end(levels)) return PXA_STATUS_UNSUPPORTED;
    if (profile == 100) {
        if (b.ue() != 1 || b.ue() != 0 || b.ue() != 0) return PXA_STATUS_UNSUPPORTED;
        b.get(1);
        if (b.get(1)) for (unsigned i = 0; i < 8; ++i) if (b.get(1)) {
            int last = 8, next = 8;
            for (unsigned j = 0; j < (i < 6 ? 16u : 64u); ++j) {
                if (next) next = (last + b.se() + 256) & 255;
                last = next ? next : last;
            }
        }
    }
    if (b.ue() > 12) return PXA_STATUS_UNSUPPORTED;
    unsigned poc = b.ue();
    if (!poc) { if (b.ue() > 12) return PXA_STATUS_UNSUPPORTED; }
    else if (poc == 1) {
        b.get(1); b.se(); b.se(); unsigned cycle = b.ue();
        if (cycle > 255) return PXA_STATUS_PROTOCOL_ERROR;
        for (unsigned i = 0; i < cycle; ++i) b.se();
    } else if (poc != 2) return PXA_STATUS_PROTOCOL_ERROR;
    if (b.ue() > 16) return PXA_STATUS_UNSUPPORTED;
    b.get(1);
    uint32_t mbw = b.ue()+1, mbh = b.ue()+1;
    if (!b.get(1)) return PXA_STATUS_UNSUPPORTED;
    b.get(1);
    uint32_t left = 0, right = 0, top = 0, bottom = 0;
    if (b.get(1)) { left=b.ue(); right=b.ue(); top=b.ue(); bottom=b.ue(); }
    if (b.bad || mbw > 128 || mbh > 128 || (left+right)*2 >= mbw*16 || (top+bottom)*2 >= mbh*16)
        return PXA_STATUS_PROTOCOL_ERROR;
    const uint32_t w=mbw*16-(left+right)*2, h=mbh*16-(top+bottom)*2;
    if (w > max_width || h > max_height || mbw*mbh > ((uint32_t(max_width)+15)/16)*((uint32_t(max_height)+15)/16))
        return PXA_STATUS_LIMIT_EXCEEDED;
    return PXA_STATUS_OK;
}
inline size_t start_code(const uint8_t *p, size_t n, size_t at) {
    if (at > n || n-at < 3) return n;
    size_t pos = at + 2;
    while (pos < n) {
        const auto *one = static_cast<const uint8_t*>(std::memchr(p+pos, 1, n-pos));
        if (!one) break;
        pos = size_t(one-p);
        if (!one[-1] && !one[-2])
            return pos - (pos-at >= 3 && !one[-3] ? 3 : 2);
        ++pos;
    }
    return n;
}
inline pxa_status_t validate_annexb(const uint8_t *p, size_t n, uint16_t max_width, uint16_t max_height) {
    size_t at = start_code(p,n,0);
    if (at != 0) return PXA_STATUS_PROTOCOL_ERROR;
    while (at < n) {
        size_t begin = at + (p[at+2] == 1 ? 3 : 4);
        size_t end = start_code(p,n,begin);
        if (begin >= end || (p[begin] & 0x80)) return PXA_STATUS_PROTOCOL_ERROR;
        if ((p[begin]&31) == 7) { auto status=check_sps(p+begin,end-begin,max_width,max_height); if (status) return status; }
        at = end;
    }
    return PXA_STATUS_OK;
}
}
