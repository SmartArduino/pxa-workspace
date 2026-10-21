#include "doit_gamekit_1_config.h"
#include "doit_gamekit_1_rotation.h"
#include <cassert>
#include <vector>

using namespace doit_gamekit_1;
int main() {
    static_assert(kNativeWidth == 320 && kNativeHeight == 240);
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
                          width, height, scale, LandscapeRotation::None);
        assert(wire.front() == 0xa55a && wire.back() == 0xa55a);
        const auto* bytes = reinterpret_cast<const uint8_t*>(wire.data() + 1);
        for (int y = 0; y < kHeight; ++y)
            for (int x = 0; x < kWidth; ++x) {
                const uint16_t expected = source[(y / scale) * stride + x / scale];
                const int index = (y * kWidth + x) * 2;
                // Exactly the reference driver's high-byte/low-byte row-major stream.
                assert(bytes[index] == uint8_t(expected >> 8));
                assert(bytes[index + 1] == uint8_t(expected));
            }
        CaptureRotatedRgb565(wire.data() + 1, capture.data(),
                            kWidth, kHeight, LandscapeRotation::None);
        assert(capture[0] == 0xf800 && capture[scale] == 0x07e0 && capture[2 * scale] == 0x001f);
        const uint16_t patch[] = {0xffff, 0x0000, 0xf800, 0x001f};
        RotateRgb565Patch(patch, 2, wire.data() + 1, kWidth, kHeight,
                         kWidth - 1, kHeight - 1, 2, 2, LandscapeRotation::None);
        assert(wire[kWidth * kHeight] == 0xffff);
        assert(wire.front() == 0xa55a && wire.back() == 0xa55a);
    }
}
