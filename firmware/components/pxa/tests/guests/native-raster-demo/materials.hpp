// SPDX-License-Identifier: MIT
#pragma once

#include "art/baked_textures.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace raster_demo {

enum Material : std::uint8_t {
    slate, brass, enamel, moonstone, grille, glow, twilight_left, twilight_right,
    texture_count,
};

inline constexpr auto light_levels = baked::light_levels;
inline constexpr auto& palette = baked::palette;
constexpr std::size_t max_texture_bytes = 256 * 128;

inline constexpr auto grille_pixels = [] {
    std::array<unsigned char, 32 * 32> texels{};
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 32; ++x) {
            if (y < 3 || y > 28 || x < 2 || x > 29)
                texels[y * 32 + x] = y == 0 || y == 29 ? 246 : 245;
            else if ((x + y) % 13 < 2 || (x - y + 39) % 13 < 2)
                texels[y * 32 + x] = 245;
        }
    }
    return texels;
}();

inline constexpr auto glow_pixels = [] {
    std::array<unsigned char, 64 * 64> texels{};
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            const int dx = x < 32 ? 31 - x : x - 32;
            const int dy = y < 32 ? 31 - y : y - 32;
            const int radius = dx * dx + dy * dy;
            const int ray = dx < dy ? dx * 7 + dy : dy * 7 + dx;
            const int orb_light = 9 - radius / 18;
            const int ray_light = 8 - ray / 4;
            const int intensity = orb_light > ray_light ? orb_light : ray_light;
            if (intensity > 0)
                texels[y * 64 + x] = static_cast<unsigned char>(246 + intensity);
        }
    }
    return texels;
}();

struct Texture {
    const unsigned char* indices;
    std::uint16_t width;
    std::uint16_t height;

    std::span<const std::byte> pixels() const {
        return std::as_bytes(std::span(indices, std::size_t(width) * height));
    }
};

inline constexpr std::array<Texture, texture_count> textures{{
    {baked::slate, 64, 64}, {baked::brass, 64, 64},
    {baked::enamel, 64, 64}, {baked::moonstone, 128, 128},
    {grille_pixels.data(), 32, 32}, {glow_pixels.data(), 64, 64},
    {baked::twilight_left, 256, 128}, {baked::twilight_right, 256, 128},
}};

static_assert(sizeof(baked::slate) == 64 * 64 + 1 && sizeof(baked::brass) == 64 * 64 + 1);
static_assert(sizeof(baked::enamel) == 64 * 64 + 1 && sizeof(baked::moonstone) == 128 * 128 + 1);
static_assert(sizeof(baked::twilight_left) == max_texture_bytes + 1 &&
              sizeof(baked::twilight_right) == max_texture_bytes + 1);
static_assert(max_texture_bytes >= sizeof(palette));

}  // namespace raster_demo
