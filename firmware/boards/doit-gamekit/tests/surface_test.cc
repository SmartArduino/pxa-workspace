#include "doit_gamekit_surface.h"
#include <array>
#include <cassert>

using namespace doit_gamekit;
static void CheckDirectEligibility() {
    std::array<uint16_t, kWidth * kHeight> pixels{};
    for (const int scale : {1, 2, 4}) {
        pxa_esp_surface_frame_t frame{};
        frame.pixels = reinterpret_cast<const uint8_t*>(pixels.data());
        frame.width = kWidth / scale; frame.height = kHeight / scale;
        frame.stride_bytes = frame.width * 2;
        frame.visible = 1; frame.format = PXA_SURFACE_FORMAT_RGB565;
        frame.flags = PXA_SURFACE_FLAG_PREFER_DIRECT_SCANOUT;
        assert(CanPresentDirectly(frame, false, false));
        assert(!CanPresentDirectly(frame, true, false)); // trusted modal
        assert(!CanPresentDirectly(frame, false, true)); // virtual mouse
        frame.stride_bytes += 4;
        assert(CanPresentDirectly(frame, false, false)); // padded rows supported
        auto rejected = frame;
        rejected.stride_bytes = frame.width * 2 - 1;
        assert(!CanPresentDirectly(rejected, false, false));
        rejected = frame; ++rejected.stride_bytes;
        assert(!CanPresentDirectly(rejected, false, false));
        rejected = frame; ++rejected.pixels;
        assert(!CanPresentDirectly(rejected, false, false));
        rejected = frame; rejected.flags = 0;
        assert(!CanPresentDirectly(rejected, false, false));
        rejected = frame; rejected.x = 1;
        assert(!CanPresentDirectly(rejected, false, false));
        rejected = frame; --rejected.height;
        assert(!CanPresentDirectly(rejected, false, false));
        rejected = frame; rejected.format = PXA_SURFACE_FORMAT_ARGB8888_PREMULTIPLIED;
        assert(!CanPresentDirectly(rejected, false, false));
        rejected = frame; rejected.opaque_ui_region_count = 1;
        assert(!CanPresentDirectly(rejected, false, false));
        const uint8_t alpha = 255;
        frame.ui_alpha_plane = {.pixels = pixels.data(), .alpha = &alpha,
            .pixel_stride_bytes = 2, .alpha_stride_bytes = 1,
            .x = 0, .y = 0, .width = 1, .height = 1, .opacity = 255, .visible = 1, .revision = 0};
        assert(!CanPresentDirectly(frame, false, false));
        frame.suppress_guest_alpha = 1;
        assert(!CanPresentDirectly(frame, false, false)); // conservative fallback
        frame.system_alpha_plane = frame.ui_alpha_plane;
        frame.ui_alpha_plane.visible = 0;
        assert(!CanPresentDirectly(frame, false, false));
        frame.system_alpha_plane.opacity = 0;
        assert(CanPresentDirectly(frame, false, false));
    }
}

int main() {
    CheckDirectEligibility();
    std::array<uint16_t, 160 * 120> source{};
    source[0] = 0xf800; source[1] = 0x001f;
    pxa_esp_surface_frame_t frame{};
    frame.pixels = reinterpret_cast<const uint8_t*>(source.data());
    frame.width = 160; frame.height = 120; frame.stride_bytes = 320;
    frame.visible = 1; frame.format = PXA_SURFACE_FORMAT_RGB565;
    std::array<uint16_t, 12> tile;
    tile.fill(0x07e0);
    Compose(frame, tile.data(), 0, 0, 4, 2, 6);
    assert(tile[0] == 0xf800 && tile[1] == 0xf800 && tile[2] == 0x001f && tile[3] == 0x001f);
    assert(tile[6] == 0xf800 && tile[7] == 0xf800 && tile[8] == 0x001f && tile[9] == 0x001f);
    assert(tile[4] == 0x07e0 && tile[5] == 0x07e0 && tile[10] == 0x07e0); // padded stride

    frame.opaque_ui_region_count = 1;
    frame.opaque_ui_regions[0] = {0, 0, 1, 1};
    tile.fill(0x07e0);
    Compose(frame, tile.data(), 0, 0, 4, 2, 6);
    assert(tile[0] == 0x07e0 && tile[1] == 0x07e0 && tile[2] == 0x001f);
    const uint16_t white = 0xffff, black = 0;
    const uint8_t opaque = 255;
    frame.ui_alpha_plane = {.pixels = &white, .alpha = &opaque,
        .pixel_stride_bytes = 2, .alpha_stride_bytes = 1,
        .x = 0, .y = 0, .width = 1, .height = 1, .opacity = 255, .visible = 1, .revision = 0};
    tile.fill(0x07e0);
    Compose(frame, tile.data(), 0, 0, 4, 2, 6);
    assert(tile[0] == white && tile[1] == 0x07e0);
    frame.suppress_guest_alpha = true;
    frame.system_alpha_plane = frame.ui_alpha_plane;
    frame.system_alpha_plane.pixels = &black;
    tile.fill(0x07e0);
    Compose(frame, tile.data(), 0, 0, 4, 2, 6);
    assert(tile[0] == black && tile[1] == 0x07e0); // trusted overlay remains

    // A centered 10x10 Surface scales 4x to (140,100), leaving letterbox pixels intact.
    frame = {};
    frame.pixels = reinterpret_cast<const uint8_t*>(source.data());
    frame.width = 10; frame.height = 10; frame.stride_bytes = 320;
    frame.visible = 1; frame.format = PXA_SURFACE_FORMAT_RGB565;
    tile.fill(0x07e0);
    Compose(frame, tile.data(), 139, 99, 3, 3, 4);
    assert(tile[0] == 0x07e0 && tile[4] == 0x07e0 && tile[5] == 0xf800 && tile[6] == 0xf800);

    std::array<uint32_t, 320 * 240> argb{};
    argb[321] = 0x80800000; // premultiplied half-red
    frame.pixels = reinterpret_cast<const uint8_t*>(argb.data());
    frame.width = 320; frame.height = 240; frame.stride_bytes = 320 * 4;
    frame.format = PXA_SURFACE_FORMAT_ARGB8888_PREMULTIPLIED;
    frame.x = -1; frame.y = -1;
    tile.fill(0x001f);
    Compose(frame, tile.data(), 0, 0, 2, 2, 4);
    assert(tile[0] == 0x800f && tile[1] == 0x001f && tile[4] == 0x001f);
    frame.visible = 0;
    tile.fill(0x07e0);
    Compose(frame, tile.data(), 0, 0, 2, 2, 4);
    assert(tile[0] == 0x07e0);
}
