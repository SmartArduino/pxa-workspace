#include "pxa/display_rotation.h"
#include "pxa/pxa_surface_transform.h"
#include <cassert>
#include <cstdio>
#include <vector>

int main() {
    constexpr uint32_t panel_width = 240, panel_height = 296;
    for (uint8_t rotation = 0; rotation < 4; ++rotation) {
        const auto width = pxa_display_logical_width(296, 240, rotation);
        const auto height = pxa_display_logical_height(296, 240, rotation);
        for (uint8_t scale : {1, 2, 4}) for (bool swapped : {false, true}) {
            const auto sw = width / scale, sh = height / scale;
            std::vector<uint16_t> source(sw * sh), stage(width * height), panel(width * height);
            for (uint32_t y = 0; y < sh; ++y) for (uint32_t x = 0; x < sw; ++x) {
                uint16_t pixel = uint16_t(x * 251 + y * 17 + 3);
                source[y * sw + x] = swapped ? __builtin_bswap16(pixel) : pixel;
            }
            // The existing 270-named wire kernel applies the panel mounting
            // correction. App rotation is composed after it, in panel order.
            assert(pxa_surface_transform_rgb565_270_columns(source.data(), stage.data(),
                sw, sh, 0, sw, scale, swapped));
            pxa_display_rgb565_to_native(stage.data(), panel.data(), panel_width, panel_height, rotation);
            const uint8_t total = (1 + rotation) & 3;
            for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x) {
                uint32_t nx, ny, lx, ly;
                pxa_display_logical_to_native(panel_width, panel_height, total, x, y, &nx, &ny);
                assert(nx < panel_width && ny < panel_height);
                assert(__builtin_bswap16(panel[ny * panel_width + nx]) ==
                    uint16_t((x / scale) * 251 + (y / scale) * 17 + 3));
                pxa_display_native_to_logical(panel_width, panel_height, total, nx, ny, &lx, &ly);
                assert(lx == x && ly == y);
            }
        }
    }
    puts("Pai-touch rotation: fixed mounting + four App angles, all pixels/touch inverses, 1x/2x/4x and both byte orders passed");
}
