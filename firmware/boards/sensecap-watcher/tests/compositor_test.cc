#include <cassert>
#include <cstdio>
#include <vector>
#include "../src/watcher_pxa_surface.cc"

namespace {
pxa_esp_surface_frame_t Frame(const std::vector<uint16_t>& pixels,
                            uint16_t width, uint16_t height, uint32_t stride) {
    pxa_esp_surface_frame_t frame{};
    frame.pixels = reinterpret_cast<const uint8_t*>(pixels.data());
    frame.width = width;
    frame.height = height;
    frame.stride_bytes = stride;
    frame.visible = true;
    frame.format = PXA_SURFACE_FORMAT_RGB565;
    frame.frame_id = 42;
    frame.input_timestamp_us = 123;
    return frame;
}

void TestCopy(uint16_t width, uint16_t height, unsigned scale) {
    const unsigned stride = width + 3;
    std::vector<uint16_t> pixels(stride * height);
    for (size_t i = 0; i < pixels.size(); ++i) pixels[i] = i * 17;
    const auto frame = Frame(pixels, width, height, stride * 2);
    const int left = (412 - width * scale) / 2;
    const int top = (412 - height * scale) / 2;
    const lv_area_t area{left - 2, top - 2, left + 9, top + 7};
    std::vector<uint16_t> output(12 * 10 + 2, 0x1234);
    const auto info = watcher_pxa_surface::ComposeFrame(
        frame, &area, reinterpret_cast<uint8_t*>(output.data() + 1));
    assert(info.frame_id == 42 && info.input_timestamp_us == 123);
    assert(output.front() == 0x1234 && output.back() == 0x1234);
    for (int y = area.y1; y <= area.y2; ++y) {
        for (int x = area.x1; x <= area.x2; ++x) {
            const auto value = output[1 + (y-area.y1)*12 + x-area.x1];
            if (x < left || y < top) assert(value == 0x1234);
            else assert(value == __builtin_bswap16(
                pixels[(y-top)/scale*stride + (x-left)/scale]));
        }
    }
}

void TestEligibilityAndAlpha() {
    std::vector<uint16_t> pixels(412*412, 0x07e0);
    auto frame = Frame(pixels, 412, 412, 824);
    assert(watcher_pxa_surface::DirectFrameEligible(frame));
    frame.x = 1;
    assert(!watcher_pxa_surface::DirectFrameEligible(frame));
    frame.x = 0;
    frame.opaque_ui_region_count = 1;
    assert(!watcher_pxa_surface::DirectFrameEligible(frame));
    frame.opaque_ui_region_count = 0;
    frame.stride_bytes = 822;
    assert(!watcher_pxa_surface::DirectFrameEligible(frame));
    frame.stride_bytes = 824;
    uint16_t colors[]{0, 0xf800, 0x8000};
    uint8_t alpha[]{0, 255, 128};
    auto& plane = frame.ui_alpha_plane;
    plane.visible = true;
    plane.opacity = 255;
    plane.x = 50; plane.y = 50; plane.width = 3; plane.height = 1;
    plane.pixels = colors; plane.alpha = alpha;
    plane.pixel_stride_bytes = 6; plane.alpha_stride_bytes = 3;
    assert(watcher_pxa_surface::DirectFrameEligible(frame));
    const lv_area_t area{49, 50, 53, 50};
    uint16_t out[5]{};
    watcher_pxa_surface::ComposeFrame(frame, &area,
        reinterpret_cast<uint8_t*>(out));
    assert(out[0] == __builtin_bswap16(0x07e0));
    assert(out[1] == __builtin_bswap16(0x07e0));
    assert(out[2] == __builtin_bswap16(0xf800));
    assert(out[3] != out[1] && out[3] != out[2]);
    assert(out[4] == out[0]);
    plane.alpha = nullptr;
    assert(!watcher_pxa_surface::DirectFrameEligible(frame));
    frame.suppress_guest_alpha = true;
    assert(watcher_pxa_surface::DirectFrameEligible(frame));
    frame.system_alpha_plane = plane;
    assert(!watcher_pxa_surface::DirectFrameEligible(frame));
    auto small = Frame(pixels, 296, 240, 592);
    assert(!watcher_pxa_surface::DirectFrameEligible(small));
    auto half = Frame(pixels, 206, 206, 412);
    assert(watcher_pxa_surface::DirectFrameEligible(half));
}

void TestOverlayMatchesGeneralPath() {
    std::vector<uint16_t> pixels(412 * 412);
    for (size_t i = 0; i < pixels.size(); ++i) pixels[i] = i * 31;
    uint16_t colors[]{0, 0x4000, 0xf800, 0x0010};
    uint8_t alpha[]{0, 64, 255, 128};
    for (uint16_t width : {uint16_t(412), uint16_t(206)}) {
        auto frame = Frame(pixels, width, width, width * 2);
        for (uint8_t opacity : {uint8_t(0), uint8_t(127), uint8_t(255)}) {
            auto& plane = frame.ui_alpha_plane;
            plane.visible = true; plane.opacity = opacity;
            plane.pixels = colors; plane.alpha = alpha;
            plane.width = 2; plane.height = 2; plane.x = 53; plane.y = 51;
            plane.pixel_stride_bytes = 4; plane.alpha_stride_bytes = 2;
            frame.system_alpha_plane = plane;
            frame.system_alpha_plane.x = 54;
            frame.system_alpha_plane.y = 52;
            const lv_area_t area{50, 50, 56, 55};
            uint16_t fast[42]{}, general[42]{};
            watcher_pxa_surface::ComposeFrame(frame, &area,
                reinterpret_cast<uint8_t*>(fast));
            // An opaque region outside the area forces the original general
            // compositor, giving an independent reference for order/clipping.
            frame.opaque_ui_region_count = 1;
            frame.opaque_ui_regions[0] = {0, 0, 1, 1};
            watcher_pxa_surface::ComposeFrame(frame, &area,
                reinterpret_cast<uint8_t*>(general));
            assert(std::memcmp(fast, general, sizeof(fast)) == 0);
            frame.opaque_ui_region_count = 0;
        }
        std::vector<uint16_t> full(412*412);
        frame.ui_alpha_plane = {}; frame.system_alpha_plane = {};
        const lv_area_t area{0, 0, 411, 411};
        watcher_pxa_surface::ComposeFrame(frame, &area,
            reinterpret_cast<uint8_t*>(full.data()));
        const unsigned scale = 412 / width;
        for (unsigned y = 0; y < 412; ++y)
            for (unsigned x = 0; x < 412; ++x)
                assert(full[y*412+x] == __builtin_bswap16(pixels[y/scale*width+x/scale]));
        // Native scanout packs eight rows at a time, with four final rows.
        // Band coordinates must preserve the same pixels as full composition.
        std::vector<uint16_t> bands(412 * 412);
        for (int y = 0; y < 412; y += 8) {
            const lv_area_t band{0, y, 411, std::min(y + 7, 411)};
            watcher_pxa_surface::ComposeFrame(frame, &band,
                reinterpret_cast<uint8_t*>(bands.data() + y * 412));
        }
        assert(full == bands);
    }
}
}

int main() {
    TestCopy(412, 412, 1);
    TestCopy(206, 206, 2);
    TestCopy(296, 240, 1);
    TestEligibilityAndAlpha();
    TestOverlayMatchesGeneralPath();
    std::puts("Watcher compositor tests passed");
}
