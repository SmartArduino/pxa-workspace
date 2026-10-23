#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace epdiy_v7 {
constexpr int kNativeWidth = 1216, kNativeHeight = 684;
constexpr size_t kFrameBytes = size_t(kNativeWidth) * kNativeHeight / 2;

// Absolute display coordinates anchor the dither, so partial tiles never
// change their quantization when LVGL changes the tile boundaries.
inline uint8_t Gray565(uint16_t rgb, int x = 0, int y = 0, bool dither = false) {
    const int r = ((rgb >> 11) * 255 + 15) / 31;
    const int g = (((rgb >> 5) & 63) * 255 + 31) / 63;
    const int b = ((rgb & 31) * 255 + 15) / 31;
    int luma = (77 * r + 150 * g + 29 * b + 128) >> 8;
    if (dither) {
        constexpr uint8_t bayer[4][4] = {{0,8,2,10},{12,4,14,6},{3,11,1,9},{15,7,13,5}};
        luma += int(bayer[y & 3][x & 3]) - 8;
    }
    return uint8_t(std::clamp((luma + 8) / 17, 0, 15));
}
inline uint8_t GetPixel(const uint8_t* frame, int x, int y) {
    return (frame[size_t(y) * kNativeWidth / 2 + x / 2] >> ((x & 1) * 4)) & 15;
}
inline bool PutPixel(uint8_t* frame, int x, int y, uint8_t gray) {
    auto& byte = frame[size_t(y) * kNativeWidth / 2 + x / 2];
    const int shift = (x & 1) * 4;
    const uint8_t next = (byte & ~(15 << shift)) | ((gray & 15) << shift);
    const bool changed = next != byte;
    byte = next;
    return changed;
}
inline bool PutLogical(uint8_t* frame, int x, int y, uint8_t gray, bool portrait,
                        bool inverted = false) {
    const int nx = portrait ? kNativeWidth - 1 - y : x;
    const int ny = portrait ? x : y;
    return PutPixel(frame, inverted ? kNativeWidth - 1 - nx : nx,
                     inverted ? kNativeHeight - 1 - ny : ny, gray);
}
inline uint8_t GetLogical(const uint8_t* frame, int x, int y, bool portrait,
                           bool inverted = false) {
    const int nx = portrait ? kNativeWidth - 1 - y : x;
    const int ny = portrait ? x : y;
    return GetPixel(frame, inverted ? kNativeWidth - 1 - nx : nx,
                      inverted ? kNativeHeight - 1 - ny : ny);
}
inline uint16_t GrayRgb565(uint8_t gray) {
    const unsigned v = gray * 17;
    return uint16_t(((v * 31 + 127) / 255 << 11) |
                    ((v * 63 + 127) / 255 << 5) | ((v * 31 + 127) / 255));
}
struct Difference {
    int x = 0, y = 0, width = 0, height = 0;
    uint32_t pixels = 0;
    bool monochrome = true;
};
inline Difference Compare(const uint8_t* next, const uint8_t* previous) {
    Difference result;
    int left = kNativeWidth, right = -1, top = kNativeHeight, bottom = -1;
    for (int y = 0; y < kNativeHeight; ++y) {
        for (int b = 0; b < kNativeWidth / 2; ++b) {
            const size_t i = size_t(y) * kNativeWidth / 2 + b;
            if (next[i] == previous[i]) continue;
            for (int nibble = 0; nibble < 2; ++nibble) {
                const uint8_t a = (next[i] >> (nibble * 4)) & 15;
                const uint8_t p = (previous[i] >> (nibble * 4)) & 15;
                if (a == p) continue;
                const int x = b * 2 + nibble;
                left = std::min(left, x); right = std::max(right, x);
                top = std::min(top, y); bottom = y;
                ++result.pixels;
                result.monochrome &= (a == 0 || a == 15) && (p == 0 || p == 15);
            }
        }
    }
    if (right >= left) {
        // EPDiy's cropped diff assembly operates on packed pixel pairs.
        result.x = left & ~1; result.y = top;
        result.width = (right | 1) - result.x + 1;
        result.height = bottom - top + 1;
    }
    return result;
}
enum class RefreshMode { None, Du, Gl16, Gc16, Clear };
inline RefreshMode Choose(const Difference& diff, bool initialized,
                          unsigned partials, unsigned clean_every,
                          unsigned gc16_percent, bool fast_mono) {
    if (!initialized) return RefreshMode::Clear;
    if (!diff.pixels) return RefreshMode::None;
    if (partials >= clean_every) return RefreshMode::Clear;
    if (uint64_t(diff.pixels) * 100 >= uint64_t(kNativeWidth) * kNativeHeight * gc16_percent)
        return RefreshMode::Gc16;
    return fast_mono && diff.monochrome ? RefreshMode::Du : RefreshMode::Gl16;
}
}
