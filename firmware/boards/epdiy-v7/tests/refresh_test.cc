#include "epdiy_refresh.h"
#include "epdiy_capture.h"
#include <cassert>
#include <cstdio>
#include <random>
#include <vector>
using namespace epdiy_v7;

int main() {
    assert(Gray565(0) == 0 && Gray565(0xffff) == 15);
    assert(Gray565(0x07e0) > Gray565(0xf800));
    assert(Gray565(0xf800) > Gray565(0x001f));
    for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) {
        assert(Gray565(0, x, y, true) == 0);
        assert(Gray565(0xffff, x, y, true) == 15);
    }
    for (uint8_t gray = 0; gray < 16; ++gray)
        assert(Gray565(GrayRgb565(gray)) == gray);
    std::vector<uint8_t> old(kFrameBytes, 0xff), next(old);
    auto diff = Compare(next.data(), old.data());
    assert(!diff.pixels && !diff.width && !diff.height);
    assert(Choose(diff, false, 0, 20, 40, true) == RefreshMode::Clear);
    assert(Choose(diff, true, 20, 20, 40, true) == RefreshMode::None);
    assert(PutPixel(next.data(), 1215, 683, 0));
    assert(GetPixel(next.data(), 1214, 683) == 15);
    assert(!PutPixel(next.data(), 1215, 683, 0));
    diff = Compare(next.data(), old.data());
    assert(diff.x == 1214 && diff.y == 683 && diff.width == 2 && diff.height == 1 && diff.pixels == 1);
    assert(Choose(diff, true, 0, 20, 40, true) == RefreshMode::Du);
    assert(Choose(diff, true, 0, 20, 40, false) == RefreshMode::Gl16);
    assert(Choose(diff, true, 20, 20, 40, true) == RefreshMode::Clear);
    PutPixel(next.data(), 1215, 683, 7);
    diff = Compare(next.data(), old.data());
    assert(!diff.monochrome && Choose(diff, true, 0, 20, 40, true) == RefreshMode::Gl16);
    // A gray -> black transition must not accidentally select DU.
    old = next; PutPixel(next.data(), 1215, 683, 0);
    assert(!Compare(next.data(), old.data()).monochrome);
    std::fill(next.begin(), next.end(), 0);
    std::fill(old.begin(), old.end(), 0xff);
    diff = Compare(next.data(), old.data());
    assert(diff.pixels == kNativeWidth * kNativeHeight);
    assert(Choose(diff, true, 0, 20, 40, true) == RefreshMode::Gc16);
    // Verify the four portrait corners against independently calculated native
    // positions, plus every logical pixel against its round-trip coordinates.
    std::fill(next.begin(), next.end(), 0xff);
    PutLogical(next.data(), 0, 0, 1, true);
    PutLogical(next.data(), 683, 0, 2, true);
    PutLogical(next.data(), 0, 1215, 3, true);
    PutLogical(next.data(), 683, 1215, 4, true);
    assert(GetPixel(next.data(), 1215, 0) == 1);
    assert(GetPixel(next.data(), 1215, 683) == 2);
    assert(GetPixel(next.data(), 0, 0) == 3);
    assert(GetPixel(next.data(), 0, 683) == 4);
    std::fill(next.begin(), next.end(), 0xff);
    PutLogical(next.data(), 0, 0, 1, true, true);
    PutLogical(next.data(), 683, 0, 2, true, true);
    PutLogical(next.data(), 0, 1215, 3, true, true);
    PutLogical(next.data(), 683, 1215, 4, true, true);
    assert(GetPixel(next.data(), 0, 683) == 1);
    assert(GetPixel(next.data(), 0, 0) == 2);
    assert(GetPixel(next.data(), 1215, 683) == 3);
    assert(GetPixel(next.data(), 1215, 0) == 4);
    assert(GetLogical(next.data(), 0, 0, true, true) == 1);
    assert(GetLogical(next.data(), 683, 1215, true, true) == 4);
    for (int y = 0; y < 1216; ++y) for (int x = 0; x < 684; ++x) {
        const uint8_t value = (x + y) & 15;
        PutLogical(next.data(), x, y, value, true);
        assert(GetLogical(next.data(), x, y, true) == value);
    }
    // Sparse random updates include both nibbles and screen edges. Compare the
    // byte-oriented diff against a separate pixel-level oracle.
    std::mt19937 random(0x7e9d);
    old = next;
    for (int n = 0; n < 500; ++n)
        PutPixel(next.data(), random() % kNativeWidth, random() % kNativeHeight, random() % 16);
    diff = Compare(next.data(), old.data());
    unsigned changed = 0;
    for (int y = 0; y < kNativeHeight; ++y) for (int x = 0; x < kNativeWidth; ++x) {
        if (GetPixel(next.data(), x, y) == GetPixel(old.data(), x, y)) continue;
        ++changed;
        assert(x >= diff.x && x < diff.x + diff.width);
        assert(y >= diff.y && y < diff.y + diff.height);
    }
    assert(changed == diff.pixels);
    // Exercise the actual capture encoder with long runs and random pixels,
    // checking every decoded pixel independently in both orientations.
    std::vector<uint8_t> encoded(2 * kFrameBytes), decoded;
    for (bool portrait : {false, true}) for (bool inverted : {false, true}) {
        for (bool solid : {false, true}) {
            auto frame = solid ? old : next;
            if (solid) std::fill(frame.begin(), frame.end(), 0xff);
            const size_t count = CaptureGray4Rle(frame.data(), encoded.data(), encoded.size(), portrait, inverted);
            assert(count && count % 2 == 0);
            decoded.clear();
            for (size_t i = 0; i < count; i += 2) {
                assert(encoded[i]);
                decoded.insert(decoded.end(), encoded[i], encoded[i + 1]);
            }
            assert(decoded.size() == kFrameBytes);
            const int width = portrait ? kNativeHeight : kNativeWidth;
            const int height = portrait ? kNativeWidth : kNativeHeight;
            for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
                const uint8_t gray = (decoded[size_t(y) * width / 2 + x / 2] >> ((x & 1) * 4)) & 15;
                assert(gray == GetLogical(frame.data(), x, y, portrait, inverted));
            }
            assert(CaptureGray4Rle(frame.data(), encoded.data(), count - 1, portrait, inverted) == 0);
        }
    }
    // Worst-case alternating packed bytes need the complete bounded buffer.
    for (size_t i = 0; i < next.size(); ++i) next[i] = i & 1 ? 0xab : 0x12;
    assert(CaptureGray4Rle(next.data(), encoded.data(), encoded.size(), false, false) == encoded.size());
    puts("EPDiy gray packing, rotation, dirty bounds and refresh policy passed");
}
