#include "epdiy_config.h"
#include <array>
#include <cassert>
#include <cstdio>
using namespace epdiy_v7;
int main() {
    // Padded source rows, 4x scaling, and a tile cutting through scaled pixels.
    const std::array<uint16_t, 8> source{0xf800, 0x07e0, 0xdead, 0xbeef,
                                         0x001f, 0xffff, 0xdead, 0xbeef};
    pxa_esp_surface_frame_t frame{};
    frame.visible = 1; frame.pixels = reinterpret_cast<const uint8_t*>(source.data());
    frame.width = 2; frame.height = 2; frame.stride_bytes = 8;
    frame.format = PXA_SURFACE_FORMAT_RGB565; frame.x = 10; frame.y = 20;
    std::array<uint16_t, 12> tile;
    tile.fill(0x1234);
    Compose(frame, tile.data(), 13, 23, 2, 2, 6);
    assert(tile[0] == 0xf800 && tile[1] == 0x07e0);
    assert(tile[6] == 0x001f && tile[7] == 0xffff);
    assert(tile[2] == 0x1234 && tile[8] == 0x1234);
    frame.opaque_ui_region_count = 1;
    frame.opaque_ui_regions[0] = {0, 0, 1, 1};
    tile.fill(0x1234);
    Compose(frame, tile.data(), 10, 20, 1, 1, 6);
    assert(tile[0] == 0x1234); // trusted opaque UI remains above the app
    frame.opaque_ui_region_count = 0;
    const uint16_t guest = 0, system = 0xffff;
    const uint8_t opaque = 255;
    frame.ui_alpha_plane = {&guest, &opaque, 2, 1, 10, 20, 1, 1, 255, 1, 0};
    frame.system_alpha_plane = {&system, &opaque, 2, 1, 10, 20, 1, 1, 255, 1, 0};
    Compose(frame, tile.data(), 10, 20, 1, 1, 6);
    assert(tile[0] == system); // system plane is always last
    frame.system_alpha_plane.visible = 0;
    Compose(frame, tile.data(), 10, 20, 1, 1, 6);
    assert(tile[0] == guest);
    frame.suppress_guest_alpha = 1;
    Compose(frame, tile.data(), 10, 20, 1, 1, 6);
    assert(tile[0] == source[0]);
    // ARGB channels are already premultiplied: half white over black is gray.
    const uint32_t argb = 0x80808080;
    frame.ui_alpha_plane.visible = 0;
    frame.pixels = reinterpret_cast<const uint8_t*>(&argb);
    frame.format = PXA_SURFACE_FORMAT_ARGB8888_PREMULTIPLIED;
    frame.width = frame.height = 1; frame.stride_bytes = 4;
    tile.fill(0);
    Compose(frame, tile.data(), 10, 20, 1, 1, 6);
    assert(Gray565(tile[0]) == 8);
    // Empty, hidden and malformed frames leave LVGL's existing UI intact.
    frame.stride_bytes = 3; tile.fill(0x1234);
    Compose(frame, tile.data(), 10, 20, 1, 1, 6);
    assert(tile[0] == 0x1234);
    printf("EPDiy %dx%d Surface scaling, stride, ARGB and trusted overlays passed\n", kWidth, kHeight);
}
