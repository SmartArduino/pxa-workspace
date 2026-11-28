// SPDX-License-Identifier: MIT
#pragma once

#include <pxa/game3d.hpp>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace raster_demo {

using pxa::game3d::FixedMeshVertex;
using pxa::game3d::Projector;
using pxa::game3d::Vec3;

constexpr int texture_size = 32;
constexpr int texture_count = 5;
constexpr int light_levels = 4;
constexpr int frames_per_shot = 60;
constexpr int shot_count = 3;
constexpr int scene_frames = shot_count * frames_per_shot;
constexpr float turn = 2.0f * std::numbers::pi_v<float>;
using Texture = std::array<std::byte, texture_size * texture_size>;
using Palette = std::array<std::uint16_t, 256 * light_levels>;

inline Texture make_texture(int material) {
    Texture data;
    for (int y = 0; y < texture_size; ++y) {
        for (int x = 0; x < texture_size; ++x) {
            const int grain = (x * 73 ^ y * 29 ^ (x * y * 17)) & 7;
            int color = 0;
            switch (material) {
            case 0:  // Slate tiles
                color = x < 2 || y < 2 ? 1 : 3 + grain / 2;
                break;
            case 1:  // Amber metal panels
                color = x < 2 || y < 2 ? 17 : (x > 25 || y > 25 ? 24 : 19 + grain / 2);
                break;
            case 2:  // Illuminated rings and signage
                color = y < 4 || y > 27 ? 37 : (x % 16 < 2 ? 38 : 33 + grain / 2);
                break;
            case 3:  // Cutout metal grille
                if (x % 8 < 2 || y % 8 < 2 || x == y)
                    color = 49 + grain / 2;
                break;
            case 4: {  // Transparent ring and glowing core
                const int dx = 2 * x - 31;
                const int dy = 2 * y - 31;
                const int radius = dx * dx + dy * dy;
                if (radius < 121 || (radius > 400 && radius < 840))
                    color = radius < 80 ? 69 : 65 + (x + y) % 4;
                break;
            }
            default:
                break;
            }
            data[y * texture_size + x] = std::byte(color);
        }
    }
    return data;
}

inline Palette make_palette() {
    Palette palette{};
    constexpr std::uint32_t colors[texture_count][8] = {
        {0x233243, 0x586276, 0x526575, 0x596b7b, 0x5d7181, 0x617989, 0x637985, 0x6f8590},
        {0x3b313b, 0x824a44, 0xa8604b, 0xaa684e, 0xaf704f, 0xb37559, 0xd18d60, 0xffb56a},
        {0x164658, 0x215771, 0x255b79, 0x29647f, 0x2f7488, 0x4befd7, 0x80ffe9, 0x51b2c0},
        {0x407388, 0x5390a1, 0x62aaad, 0x78aeb3, 0xa1e1d0, 0x3f8590, 0x4daeb5, 0x4db3bb},
        {0x6be9ff, 0x8ce9ff, 0xacdfff, 0xf3a8ff, 0xf0ccff, 0xfffaff, 0xffeecc, 0xf0ffff},
    };
    for (int light = 0; light < light_levels; ++light) {
        const unsigned level = 4 + light;
        for (int material = 0; material < texture_count; ++material) {
            for (int i = 0; i < 8; ++i) {
                const auto rgb = colors[material][i];
                const unsigned r = (rgb >> 16) * level / 7;
                const unsigned g = ((rgb >> 8) & 255) * level / 7;
                const unsigned b = (rgb & 255) * level / 7;
                palette[256 * light + 16 * material + 1 + i] =
                    ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
            }
        }
    }
    return palette;
}

inline Vec3 minus(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline Vec3 unit(Vec3 v) {
    const float length = std::sqrt(dot(v, v));
    return {v.x / length, v.y / length, v.z / length};
}

struct Camera {
    Vec3 position, right, up, forward;

    Vec3 to_view(Vec3 p) const {
        const Vec3 v = minus(p, position);
        return {dot(v, right), -dot(v, up), dot(v, forward)};
    }
};

inline Camera camera_at(int frame) {
    const int shot = frame / frames_per_shot;
    const float t = (frame % frames_per_shot) / float(frames_per_shot);
    Vec3 eye, aim;
    if (shot == 0) {
        const float angle = (t - .5f) * 1.5f;
        eye = {9.4f * std::sin(angle), 4.2f + t * 1.1f, -9.4f * std::cos(angle)};
        aim = {0, 2.1f, 1.8f};
    } else if (shot == 1) {
        eye = {-3.8f + 6.7f * t, 1.65f + .15f * std::sin(t * turn), -7.2f + 7.0f * t};
        aim = {2.6f * std::sin(t * turn + .5f), 2.4f, 5.5f};
    } else {
        const float angle = .36f + t * 1.85f;
        eye = {9.0f * std::cos(angle), 7.1f + t * 2.5f, 3.1f + 6.2f * std::sin(angle)};
        aim = {-1.2f + t * 2.4f, 1.0f, 1.0f};
    }
    const Vec3 forward = unit(minus(aim, eye));
    const Vec3 right = unit(cross({0, 1, 0}, forward));
    return {eye, right, cross(forward, right), forward};
}

struct FrameStats {
    unsigned source_faces = 0;
    unsigned projected_faces = 0;
    unsigned cutout_faces = 0;
    unsigned blended_faces = 0;
};

inline FrameStats draw_scene(pxa::game::Frame& target, const Projector& projector, int frame) {
    const Camera camera = camera_at(frame);
    const float time = frame * turn / scene_frames;
    FrameStats stats;
    target.clear({0x192d});
    auto draw = [&](const std::array<Vec3, 4>& points, std::uint8_t material, std::uint8_t light) {
        ++stats.source_faces;
        constexpr int uv = texture_size * 16;
        std::array<FixedMeshVertex, 4> face{{
            {camera.to_view(points[0]), 0, 0, light},
            {camera.to_view(points[1]), uv, 0, light},
            {camera.to_view(points[2]), uv, uv, light},
            {camera.to_view(points[3]), 0, uv, light},
        }};
        std::array<pxa::game::Vertex, Projector::max_polygon_vertices> wire;
        const auto count = projector.project_polygon_fixed(face, wire);
        if (!count || *count < 3) return;
        const bool cutout = material >= 3;
        const bool blend = material == 4;
        const auto before = target.bytes_used();
        target.textured_depth_polygon({material}, std::span(wire).first(*count),
            {.transparent_index0 = cutout, .blend_75 = blend});
        if (target.bytes_used() != before) {
            ++stats.projected_faces;
            stats.cutout_faces += cutout;
            stats.blended_faces += blend;
        }
    };

    // Central rotor. Each of the 240 faces samples its own repeated texture.
    constexpr int rings = 24, subdivisions = 10;
    std::array<Vec3, rings * subdivisions> rotor;
    const float sine = std::sin(time), cosine = std::cos(time);
    for (int ring = 0; ring < rings; ++ring) {
        const float angle = ring * turn / rings;
        const float sa = std::sin(angle), ca = std::cos(angle);
        for (int slice = 0; slice < subdivisions; ++slice) {
            const float b = slice * turn / subdivisions;
            const float radius = 1.50f + .42f * std::cos(b);
            const float x = radius * ca;
            const float z = .42f * std::sin(b);
            rotor[ring * subdivisions + slice] = {
                cosine * x + sine * z, 2.78f + radius * sa, 1.8f + cosine * z - sine * x,
            };
        }
    }
    for (int i = 0; i < rings; ++i) {
        for (int j = 0; j < subdivisions; ++j) {
            const int a = i * subdivisions, b = ((i + 1) % rings) * subdivisions;
            const int next = (j + 1) % subdivisions;
            draw({rotor[a+j], rotor[b+j], rotor[b+next], rotor[a+next]},
                 i % 4 == 0 ? 1 : 2, 1 + std::min(j / 3, 2));
        }
    }

    // Six courses on each of ten towers. Near faces occlude courses behind them.
    for (int tower = 0; tower < 10; ++tower) {
        const float x = (tower % 2 ? 1.f : -1.f) * (3.9f + .25f * (tower / 2 % 2));
        const float z = -4.5f + 2.55f * (tower / 2);
        const float rise = .48f + .075f * (tower % 3);
        constexpr float half = .65f;
        for (int course = 0; course < 6; ++course) {
            const float lo = course * rise, hi = lo + rise;
            const Vec3 lower[] = {{x-half,lo,z-half}, {x+half,lo,z-half},
                                  {x+half,lo,z+half}, {x-half,lo,z+half}};
            const Vec3 upper[] = {{x-half,hi,z-half}, {x+half,hi,z-half},
                                  {x+half,hi,z+half}, {x-half,hi,z+half}};
            for (int side = 0; side < 4; ++side) {
                const int next = (side + 1) & 3;
                draw({upper[side],upper[next],lower[next],lower[side]},
                     course == 3 ? 2 : 1, 3 - (side % 3));
            }
            if (course == 5)
                draw({upper[0], upper[1], upper[2], upper[3]}, 1, 3);
        }
    }

    for (int z = -7; z < 9; ++z) {
        for (int x = -8; x < 8; ++x) {
            constexpr float step = 1.26f;
            const float x0 = x * step, z0 = z * step;
            draw({Vec3{x0,0,z0}, {x0+step,0,z0}, {x0+step,0,z0+step}, {x0,0,z0+step}},
                 (x == -2 || x == 1) && z % 3 == 0 ? 2 : 0, 2 + ((x + z) & 1));
        }
    }

    for (int i = 0; i < 12; ++i) {
        const float z = (i / 2) * 2.0f - 4.0f;
        const float x = i % 2 ? 2.7f : -2.7f;
        draw({Vec3{x,1.8f,z-.55f}, {x,1.8f,z+.55f}, {x,.2f,z+.55f}, {x,.2f,z-.55f}},
             3, 3);
    }

    // Sort only the effect batch back-to-front. The opaque depth stays intact.
    struct Glow { Vec3 center; float depth, radius; };
    std::array<Glow, 20> glows;
    for (unsigned i = 0; i < glows.size(); ++i) {
        const float a = i * turn / glows.size() + time * 2;
        const float r = i < 8 ? 2.2f : 2.8f;
        const Vec3 center{std::cos(a) * r, 1.35f + .85f * std::sin(a * 3 + time),
                          1.8f + std::sin(a) * r};
        glows[i] = {center, camera.to_view(center).z, i < 8 ? .47f : .25f};
    }
    std::sort(glows.begin(), glows.end(), [](const Glow& a, const Glow& b) {
        return a.depth > b.depth;
    });
    for (const auto& glow : glows) {
        auto corner = [&](float x, float y) -> Vec3 {
            const auto c = glow.center;
            const auto a = camera.right, b = camera.up;
            return {c.x + a.x*x + b.x*y, c.y + a.y*x + b.y*y, c.z + a.z*x + b.z*y};
        };
        const float r = glow.radius;
        draw({corner(-r,r),corner(r,r),corner(r,-r),corner(-r,-r)}, 4, 3);
    }
    return stats;
}
}  // namespace raster_demo
