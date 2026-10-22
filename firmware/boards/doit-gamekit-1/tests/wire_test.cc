#include "doit_gamekit_1_config.h"
#include "doit_gamekit_1_rotation.h"
#include <cassert>
#include <vector>

using namespace doit_gamekit_1;
int main() {
    static_assert(kNativeWidth == 240 && kNativeHeight == 320);
    for (const auto rotation : {LandscapeRotation::Deg270, LandscapeRotation::Deg90})
    for (int scale : {1, 2, 4}) {
        const int width = kWidth / scale, height = kHeight / scale;
        const int stride = width + 3;
        std::vector<uint16_t> source(stride * height, 0xbeef);
        std::vector<uint16_t> wire(kWidth * kHeight + 2, 0xa55a);
        std::vector<uint16_t> capture(kWidth * kHeight);
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
                source[y * stride + x] = uint16_t((y * width + x) * 17);
        source[0] = 0xf800; source[1] = 0x07e0; source[2] = 0x001f;
        RotateScaledRgb565(source.data(), stride, wire.data() + 1,
                          width, height, scale, rotation);
        assert(wire.front() == 0xa55a && wire.back() == 0xa55a);
        const auto* bytes = reinterpret_cast<const uint8_t*>(wire.data() + 1);
        for (int y = 0; y < kHeight; ++y)
            for (int x = 0; x < kWidth; ++x) {
                const uint16_t expected = source[(y / scale) * stride + x / scale];
                const int index = 2 * (rotation == LandscapeRotation::Deg270
                    ? x * kHeight + kHeight - 1 - y : (kWidth - 1 - x) * kHeight + y);
                // Actual wire stream is high-byte first in native portrait scan order.
                assert(bytes[index] == uint8_t(expected >> 8));
                assert(bytes[index + 1] == uint8_t(expected));
            }
        CaptureRotatedRgb565(wire.data() + 1, capture.data(),
                            kWidth, kHeight, rotation);
        assert(capture[0] == 0xf800 && capture[scale] == 0x07e0 && capture[2 * scale] == 0x001f);
        const uint16_t patch[] = {0xffff, 0x0000, 0xf800, 0x001f};
        RotateRgb565Patch(patch, 2, wire.data() + 1, kWidth, kHeight,
                         kWidth - 1, kHeight - 1, 2, 2, rotation);
        CaptureRotatedRgb565(wire.data() + 1, capture.data(), kWidth, kHeight, rotation);
        assert(capture.back() == 0xffff);
        assert(wire.front() == 0xa55a && wire.back() == 0xa55a);
    }
}
