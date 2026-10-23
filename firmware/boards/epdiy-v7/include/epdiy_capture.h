#pragma once
#include "epdiy_refresh.h"

namespace epdiy_v7 {
// Lossless logical gray4, low nibble first; pairs of (run length, packed byte).
// Runs may cross row boundaries. The worst case is two bytes per pixel pair.
inline size_t CaptureGray4Rle(const uint8_t* native, uint8_t* output, size_t capacity,
                             bool portrait, bool inverted) {
    if (!native || !output) return 0;
    const int width = portrait ? kNativeHeight : kNativeWidth;
    const int height = portrait ? kNativeWidth : kNativeHeight;
    size_t written = 0;
    uint8_t value = 0, run = 0;
    auto emit = [&]() {
        if (capacity - written < 2) return false;
        output[written++] = run;
        output[written++] = value;
        return true;
    };
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; x += 2) {
        const uint8_t pair = GetLogical(native, x, y, portrait, inverted) |
            (GetLogical(native, x + 1, y, portrait, inverted) << 4);
        if (run && (pair != value || run == 255)) {
            if (!emit()) return 0;
            run = 0;
        }
        value = pair;
        ++run;
    }
    return run && !emit() ? 0 : written;
}
}
