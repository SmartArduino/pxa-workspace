#include "doit_gamekit_config.h"
#include "doit_gamekit_rotation.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <vector>

using namespace doit_gamekit;

static void KnownPattern() {
    // Logical rows [1 2 3], [4 5 6], with padding that must never reach the LCD.
    const uint16_t source[] = {1, 2, 3, 0xdead, 4, 5, 6, 0xbeef};
    std::array<uint16_t, 6> native{}, captured{};
    RotateRgb565(source, 4, native.data(), 3, 2, LandscapeRotation::Deg270);
    assert((native == std::array<uint16_t, 6>{0x0400, 0x0100, 0x0500, 0x0200, 0x0600, 0x0300}));
    CaptureRotatedRgb565(native.data(), captured.data(), 3, 2, LandscapeRotation::Deg270);
    assert((captured == std::array<uint16_t, 6>{1, 2, 3, 4, 5, 6}));
    RotateRgb565(source, 4, native.data(), 3, 2, LandscapeRotation::Deg90);
    assert((native == std::array<uint16_t, 6>{0x0300, 0x0600, 0x0200, 0x0500, 0x0100, 0x0400}));
    CaptureRotatedRgb565(native.data(), captured.data(), 3, 2, LandscapeRotation::Deg90);
    assert((captured == std::array<uint16_t, 6>{1, 2, 3, 4, 5, 6}));
}

static void CheckFrame(int width, int height, LandscapeRotation rotation) {
    const int stride = width + 3;
    constexpr uint16_t guard = 0xabad;
    std::vector<uint16_t> source(stride * height, guard);
    std::vector<uint16_t> native(width * height + 2, guard), captured(width * height + 2, guard);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            source[y * stride + x] = uint16_t((x * 151 + y * 997) ^ (x << 9));
    const auto original = source;
    RotateRgb565(source.data(), stride, native.data() + 1, width, height, rotation);
    // Validate in native row order against an independent inverse coordinate
    // mapping, including dimensions that end part way through a 16x16 tile.
    for (int ny = 0; ny < width; ++ny)
        for (int nx = 0; nx < height; ++nx) {
            const int x = rotation == LandscapeRotation::Deg270 ? ny : width - 1 - ny;
            const int y = rotation == LandscapeRotation::Deg270 ? height - 1 - nx : nx;
            const uint16_t logical = source[y * stride + x];
            const auto* wire = reinterpret_cast<const uint8_t*>(&native[1 + ny * height + nx]);
            assert(wire[0] == (logical >> 8) && wire[1] == (logical & 255));
        }
    CaptureRotatedRgb565(native.data() + 1, captured.data() + 1, width, height, rotation);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            assert(captured[1 + y * width + x] == source[y * stride + x]);
    assert(native.front() == guard && native.back() == guard);
    assert(captured.front() == guard && captured.back() == guard && source == original);

    // Diagnostic patches modify only the transmitted native image. Returning
    // to a clean rotation must remove the old HUD without touching the source.
    constexpr int patch_width = 60, patch_height = 16, patch_stride = 64;
    std::array<uint16_t, patch_stride * patch_height> patch{};
    for (int y = 0; y < patch_height; ++y)
        for (int x = 0; x < patch_width; ++x) patch[y * patch_stride + x] = uint16_t(0xf800 ^ (y * 61 + x));
    const auto clean_native = native;
    const int left = (width - patch_width) / 2;
    RotateRgb565Patch(patch.data(), patch_stride, native.data() + 1, width, height,
                     left, 0, patch_width, patch_height, rotation);
    CaptureRotatedRgb565(native.data() + 1, captured.data() + 1, width, height, rotation);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const auto expected = y < patch_height && x >= left && x < left + patch_width
                ? patch[y * patch_stride + x - left] : source[y * stride + x];
            assert(captured[1 + y * width + x] == expected);
        }
    assert(native.front() == guard && native.back() == guard && source == original);
    RotateRgb565(source.data(), stride, native.data() + 1, width, height, rotation);
    assert(native == clean_native);
}

static void CheckScaledFrame(int scale, LandscapeRotation rotation) {
    const int width = kWidth / scale, height = kHeight / scale, stride = width + 3;
    constexpr uint16_t guard = 0xa55a;
    std::vector<uint16_t> source(stride * height, guard), native(kWidth * kHeight + 2, guard);
    std::vector<uint16_t> captured(kWidth * kHeight);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) source[y * stride + x] = uint16_t(x * 311 + y * 997);
    const auto original = source;
    RotateScaledRgb565(source.data(), stride, native.data() + 1, width, height, scale, rotation);
    CaptureRotatedRgb565(native.data() + 1, captured.data(), kWidth, kHeight, rotation);
    for (int y = 0; y < kHeight; ++y)
        for (int x = 0; x < kWidth; ++x)
            assert(captured[y * kWidth + x] == source[(y / scale) * stride + x / scale]);
    assert(native.front() == guard && native.back() == guard && source == original);
}

int main() {
    KnownPattern();
    for (const auto rotation : {LandscapeRotation::Deg270, LandscapeRotation::Deg90}) {
        for (int scale : {1, 2, 4}) CheckScaledFrame(scale, rotation);
        CheckFrame(kWidth, kHeight, rotation);
        CheckFrame(37, 19, rotation);
        CheckFrame(1, 1, rotation);
        CheckFrame(1, 17, rotation);
        CheckFrame(17, 1, rotation);
    }
    std::puts("DOIT GameKit native portrait rotation tests passed");
}
