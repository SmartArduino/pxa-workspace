// SPDX-License-Identifier: Apache-2.0
#include "doit_s31_surface.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <vector>

int main() {
    // Padded source rows and a tile crossing the integer-scale boundary.
    const std::array<uint16_t, 8> source{0xf800, 0x07e0, 0xdead, 0xbeef,
                                      0x001f, 0xffff, 0xdead, 0xbeef};
    pxa_esp_surface_frame_t frame{};
    frame.visible = 1; frame.pixels = reinterpret_cast<const uint8_t*>(source.data());
    frame.width = 2; frame.height = 2; frame.stride_bytes = 8;
    frame.format = PXA_SURFACE_FORMAT_RGB565; frame.x = 10; frame.y = 20;
    std::array<uint16_t, 12> tile;
    tile.fill(0x1234);
    doit_s31::Compose(frame, tile.data(), 13, 23, 2, 2, 6);
    assert(tile[0] == 0xf800 && tile[1] == 0x07e0);
    assert(tile[6] == 0x001f && tile[7] == 0xffff);
    assert(tile[2] == 0x1234 && tile[8] == 0x1234);
    frame.opaque_ui_region_count = 1;
    frame.opaque_ui_regions[0] = {0, 0, 1, 1};
    tile.fill(0x1234);
    doit_s31::Compose(frame, tile.data(), 10, 20, 1, 1, 6);
    assert(tile[0] == 0x1234);
    frame.opaque_ui_region_count = 0;
    const uint16_t guest = 0, system = 0xffff;
    const uint8_t opaque = 255;
    frame.ui_alpha_plane = {&guest, &opaque, 2, 1, 10, 20, 1, 1, 255, 1, 0};
    frame.system_alpha_plane = {&system, &opaque, 2, 1, 10, 20, 1, 1, 255, 1, 0};
    doit_s31::Compose(frame, tile.data(), 10, 20, 1, 1, 6);
    assert(tile[0] == system);
    frame.system_alpha_plane.visible = 0;
    doit_s31::Compose(frame, tile.data(), 10, 20, 1, 1, 6);
    assert(tile[0] == guest);
    frame.suppress_guest_alpha = 1;
    doit_s31::Compose(frame, tile.data(), 10, 20, 1, 1, 6);
    assert(tile[0] == source[0]);
    const uint32_t argb = 0x80808080;
    frame.ui_alpha_plane.visible = 0;
    frame.pixels = reinterpret_cast<const uint8_t*>(&argb);
    frame.format = PXA_SURFACE_FORMAT_ARGB8888_PREMULTIPLIED;
    frame.width = frame.height = 1; frame.stride_bytes = 4;
    tile.fill(0);
    doit_s31::Compose(frame, tile.data(), 10, 20, 1, 1, 6);
    assert(tile[0] == 0x8410); // half white over black, RGB565
    frame.stride_bytes = 3; tile.fill(0x1234);
    doit_s31::Compose(frame, tile.data(), 10, 20, 1, 1, 6);
    assert(tile[0] == 0x1234);
    // Unscaled RGB565 row path: centered viewport, padded source/destination,
    // clipping outside the guest and trusted alpha composition.
    std::vector<uint16_t> rows(243 * 428, 0xdead);
    for (unsigned y = 0; y < 428; ++y)
        for (unsigned x = 0; x < 241; ++x) rows[y * 243 + x] = uint16_t(y * 241 + x);
    frame = {};
    frame.visible = 1; frame.pixels = reinterpret_cast<const uint8_t*>(rows.data());
    frame.width = 241; frame.height = 428; frame.stride_bytes = 486;
    frame.format = PXA_SURFACE_FORMAT_RGB565;
    tile.fill(0x1234);
    doit_s31::Compose(frame, tile.data(), 118, 214, 4, 2, 6);
    assert(tile[0] == 0x1234 && tile[1] == 241 && tile[3] == 243);
    assert(tile[6] == 0x1234 && tile[7] == 482 && tile[9] == 484);
    assert(tile[4] == 0x1234 && tile[10] == 0x1234);
    frame.system_alpha_plane = {&system, &opaque, 2, 1, 119, 214, 1, 1, 255, 1, 0};
    doit_s31::Compose(frame, tile.data(), 118, 214, 4, 2, 6);
    assert(tile[1] == system && tile[2] == 242);
    frame.system_alpha_plane.visible = 0;
    frame.opaque_ui_region_count = 1;
    frame.opaque_ui_regions[0] = {0, 1, 1, 1};
    tile.fill(0x1234);
    doit_s31::Compose(frame, tile.data(), 118, 214, 4, 2, 6);
    assert(tile[1] == 0x1234 && tile[2] == 242); // Region-mask fallback.
    // Full portrait 2x expansion, including vertically repeated rows and a
    // trusted plane covering only the second physical row of a guest pixel.
    std::vector<uint16_t> scaled_source(242 * 427, 0xdead), expanded(482 * 854, 0x1234);
    for (unsigned y = 0; y < 427; ++y)
        for (unsigned x = 0; x < 240; ++x) scaled_source[y * 242 + x] = uint16_t(y * 240 + x);
    frame = {}; frame.visible = 1;
    frame.pixels = reinterpret_cast<const uint8_t*>(scaled_source.data());
    frame.width = 240; frame.height = 427; frame.stride_bytes = 484;
    frame.format = PXA_SURFACE_FORMAT_RGB565;
    frame.system_alpha_plane = {&system, &opaque, 2, 1, 0, 1, 1, 1, 255, 1, 0};
    doit_s31::Compose(frame, expanded.data(), 0, 0, 480, 854, 482);
    for (unsigned y = 0; y < 854; ++y) {
        for (unsigned x = 0; x < 480; ++x)
            assert(expanded[y * 482 + x] ==
                (y == 1 && x == 0 ? system : uint16_t(y / 2 * 240 + x / 2)));
        assert(expanded[y * 482 + 480] == 0x1234 && expanded[y * 482 + 481] == 0x1234);
    }
    puts("DOIT-S31: Surface stride, scaling, premultiplied ARGB and trusted overlays passed");
}
