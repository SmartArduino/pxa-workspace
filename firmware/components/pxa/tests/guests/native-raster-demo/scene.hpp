// SPDX-License-Identifier: MIT
#pragma once

#include "materials.hpp"

#include <pxa/game3d.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace raster_demo {

using pxa::game3d::FixedMeshVertex;
using pxa::game3d::Projector;
using pxa::game3d::Vec3;

constexpr int frames_per_shot = 60;
constexpr int shot_count = 3;
constexpr int scene_frames = shot_count * frames_per_shot;
constexpr float turn = 2.0f * std::numbers::pi_v<float>;
constexpr Vec3 rotor_center{0, 2.65f, 1.25f};

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
        const float angle = (t - .5f) * 1.32f;
        eye = {8.0f * std::sin(angle), 3.55f + t * .75f, 1.4f - 8.0f * std::cos(angle)};
        aim = {0, 2.08f, 1.6f};
    } else if (shot == 1) {
        eye = {-2.75f + 4.7f * t, 1.6f + .24f * std::sin(t * turn), -7.3f + 5.5f * t};
        aim = {-.65f + .7f * std::sin(t * turn), 2.35f, 1.85f};
    } else {
        const float angle = .65f + t * 1.5f;
        eye = {8.9f * std::cos(angle), 6.1f + t * 3.0f, 1.1f + 7.6f * std::sin(angle)};
        aim = {-.45f + t * .8f, .9f, 1.4f};
    }
    const Vec3 forward = unit(minus(aim, eye));
    const Vec3 right = unit(cross({0, 1, 0}, forward));
    return {eye, right, cross(forward, right), forward};
}

inline void draw_backdrop(pxa::game::Frame& target, const Camera& camera,
                          std::uint16_t width, std::uint16_t height) {
    const auto panel_width = static_cast<std::uint16_t>((3 * width + 3) / 4);
    const auto panel_height = static_cast<std::uint16_t>(height + height / 3);
    const int horizontal_margin = 2 * panel_width - width;
    const auto x = static_cast<std::int16_t>(-horizontal_margin *
        (.5f + .46f * camera.forward.x));
    const auto y = static_cast<std::int16_t>(-static_cast<int>(height) *
        (.16f - .16f * camera.forward.y));
    const pxa::game::Sprite left{x, y, panel_width, panel_height, 0, 0, 256, 128};
    const pxa::game::Sprite right{static_cast<std::int16_t>(x + panel_width), y,
                                  panel_width, panel_height, 0, 0, 256, 128};
    target.sprites({twilight_left}, {&left, 1}, {.transparent_index0 = false});
    target.sprites({twilight_right}, {&right, 1}, {.transparent_index0 = false});
}

struct FrameStats {
    unsigned source_faces = 0;
    unsigned projected_faces = 0;
    unsigned cutout_faces = 0;
    unsigned blended_faces = 0;
};

struct Uv {
    struct Point { int u, v; };
    std::array<Point, 4> corners;

    constexpr Uv(int left = 0, int top = 0, int right = 256, int bottom = 256)
        : corners{{{left,top}, {right,top}, {right,bottom}, {left,bottom}}} {}
    constexpr Uv(Point a, Point b, Point c, Point d) : corners{{a, b, c, d}} {}
};

inline FrameStats draw_scene(pxa::game::Frame& target, const Projector& projector, int frame,
                             std::uint16_t width, std::uint16_t height) {
    const Camera camera = camera_at(frame);
    const float time = frame * turn / scene_frames;
    FrameStats stats;
    // Both panels cover the full target. The rasterizer still clears depth
    // before the first command, so a separate RGB clear would be redundant.
    draw_backdrop(target, camera, width, height);
    auto draw = [&](const std::array<Vec3, 4>& points, Material material,
                    std::uint8_t light, Uv uv = {}) {
        ++stats.source_faces;
        const auto& tex = textures[material];
        std::array<FixedMeshVertex, 4> face;
        for (std::size_t i = 0; i < face.size(); ++i) {
            const auto [u, v] = uv.corners[i];
            face[i] = {camera.to_view(points[i]),
                       static_cast<std::int16_t>(u * tex.width / 16),
                       static_cast<std::int16_t>(v * tex.height / 16), light};
        }
        std::array<pxa::game::Vertex, Projector::max_polygon_vertices> wire;
        const auto count = projector.project_polygon_fixed(face, wire);
        if (!count || *count < 3) return;
        const bool cutout = material == grille || material == glow;
        const bool blend = material == glow;
        const auto before = target.bytes_used();
        target.textured_depth_polygon({material}, std::span(wire).first(*count),
            {.affine_uv = material == moonstone, .transparent_index0 = cutout, .blend_75 = blend});
        if (target.bytes_used() != before) {
            ++stats.projected_faces;
            stats.cutout_faces += cutout;
            stats.blended_faces += blend;
        }
    };

    auto slab_top = [&](float x, float z, float y, float radius, Material material,
                        std::uint8_t light, Uv uv = {}) {
        draw({Vec3{x-radius,y,z-radius}, {x+radius,y,z-radius},
                   {x+radius,y,z+radius}, {x-radius,y,z+radius}}, material, light, uv);
    };

    // The lantern-like armillary rotor uses a continuous UV strip around each
    // sector. Directional shading follows the surface as it rotates.
    constexpr int rings = 28, subdivisions = 10;
    struct SineCosine { float sine, cosine; };
    std::array<SineCosine, subdivisions> tube;
    for (int j = 0; j < subdivisions; ++j) {
        const float b = j * turn / subdivisions;
        tube[j] = {std::sin(b), std::cos(b)};
    }
    std::array<Vec3, rings * subdivisions> rotor;
    std::array<SineCosine, rings> circle;
    const float yaw = time + .22f;
    const float sine = std::sin(yaw), cosine = std::cos(yaw);
    for (int i = 0; i < rings; ++i) {
        const float angle = i * turn / rings;
        const float sa = std::sin(angle), ca = std::cos(angle);
        circle[i] = {sa, ca};
        for (int j = 0; j < subdivisions; ++j) {
            const float radius = 1.64f + .32f * tube[j].cosine;
            const float x = radius * ca, z = .32f * tube[j].sine;
            rotor[i * subdivisions + j] = {rotor_center.x + cosine * x + sine * z,
                rotor_center.y + radius * sa, rotor_center.z + cosine * z - sine * x};
        }
    }
    for (int i = 0; i < rings; ++i) {
        const int a = i * subdivisions, b = ((i + 1) % rings) * subdivisions;
        for (int j = 0; j < subdivisions; ++j) {
            const int next = (j + 1) % subdivisions;
            const float nx = circle[i].cosine * tube[j].cosine;
            const float nz = tube[j].sine;
            const Vec3 normal{cosine * nx + sine * nz, circle[i].sine * tube[j].cosine,
                              cosine * nz - sine * nx};
            const float shading = dot(normal, {-.32f, .72f, -.58f});
            const auto light = static_cast<std::uint8_t>(std::clamp(4 + int(shading * 4), 1, 7));
            const int sector = i % 4;
            draw({rotor[a+j], rotor[b+j], rotor[b+next], rotor[a+next]},
                 i / 4 % 2 ? brass : enamel, light,
                 {sector * 64, j * 256 / subdivisions, (sector+1) * 64,
                  (j+1) * 256 / subdivisions});
        }
    }

    constexpr std::array<Vec3, 8> octagon{{
        {.38f,0,-.92f}, {.92f,0,-.38f}, {.92f,0,.38f}, {.38f,0,.92f},
        {-.38f,0,.92f}, {-.92f,0,.38f}, {-.92f,0,-.38f}, {-.38f,0,-.92f},
    }};
    struct Band { float y, radius; Material finish; };
    constexpr std::array<std::uint8_t, 8> side_light{{6, 5, 4, 3, 3, 4, 5, 6}};
    constexpr std::array<Uv::Point, 8> crown_uv{{
        {177, 9}, {247, 79}, {247, 177}, {177, 247},
        {79, 247}, {9, 177}, {9, 79}, {79, 9},
    }};
    auto pylon = [&](float x, float z, std::span<const Band> bands, Material crown) {
        auto point = [&](unsigned corner, const Band& band) -> Vec3 {
            return {x + octagon[corner].x * band.radius, band.y,
                    z + octagon[corner].z * band.radius};
        };
        for (std::size_t tier = 1; tier < bands.size(); ++tier) {
            const auto& low = bands[tier-1];
            const auto& high = bands[tier];
            for (unsigned face = 0; face < octagon.size(); ++face) {
                const unsigned next = (face + 1) % octagon.size();
                draw({point(face, high), point(next, high),
                      point(next, low), point(face, low)},
                     low.finish, side_light[face]);
            }
        }
        const auto& top = bands.back();
        for (const auto& [a, b, c, d] : {std::array{0u,1u,2u,3u}, {0u,3u,4u,7u}, {4u,5u,6u,7u}})
            draw({point(a, top), point(b, top), point(c, top), point(d, top)},
                 crown, 6, {crown_uv[a], crown_uv[b], crown_uv[c], crown_uv[d]});
    };

    // Four paired pillars define a gallery without blocking the center aisle.
    for (int i = 0; i < 8; ++i) {
        const float z = -4.3f + (i / 2) * 3.25f;
        const float x = i % 2 ? 3.65f + .32f * (i / 2 % 2) : -3.65f - .32f * (i / 2 % 2);
        const float top = 2.5f + .32f * (i / 2 % 3);
        const std::array<Band, 5> shape{{
            {0, .82f, slate}, {.35f, .64f, brass}, {top-.32f, .49f, enamel},
            {top-.15f, .67f, brass}, {top, .59f, brass},
        }};
        pylon(x, z, shape, brass);
    }
    const std::array<Band, 4> altar{{
        {.02f, .93f, slate}, {.20f, .84f, brass},
        {.39f, .68f, enamel}, {.53f, .77f, brass},
    }};
    pylon(0, rotor_center.z, altar, moonstone);

    constexpr float step = 1.25f;
    constexpr float x_min = -7 * step, x_max = 7 * step;
    constexpr float z_min = -7 * step, z_max = 9 * step;
    constexpr float chart_height = .12f, chart_radius = 2 * step;
    for (int z = -7; z < 9; ++z) {
        for (int x = -7; x < 7; ++x) {
            const float tile_x = (x + .5f) * step;
            const float tile_z = (z + .5f) * step;
            // Share the floor grid to keep the star chart continuous in the
            // shallow camera pass while keeping each affine patch small.
            if (x >= -2 && x < 2 && z >= -1 && z < 3) {
                const int u = (x + 2) * 63, v = (z + 1) * 63;
                slab_top(tile_x, tile_z, chart_height, step / 2, moonstone, 6,
                         {u, v, u + 63, v + 63});
                continue;
            }
            const bool aisle = x == -2 || x == 1;
            const bool marker = aisle && z % 3 == 0;
            const auto light = static_cast<std::uint8_t>((x + z) % 2 == 0 ? 5 : 4);
            slab_top(tile_x, tile_z, 0, step / 2, marker ? enamel : slate, light);
        }
    }

    // Dark terrace fascia and a brass edge give the platform visible thickness.
    auto rail_face = [&](float x0, float z0, float x1, float z1, float lo, float hi,
                         Material finish, std::uint8_t light, Uv uv = {}) {
        draw({Vec3{x0,hi,z0}, {x1,hi,z1}, {x1,lo,z1}, {x0,lo,z0}}, finish, light, uv);
    };
    for (int i = 0; i < 16; i += 2) {
        const float lo = z_min + i * step, hi = lo + 2 * step;
        for (float edge : {x_min, x_max})
            rail_face(edge,lo,edge,hi,-.65f,0,slate,3,{0,0,512,256});
    }
    for (int i = 0; i < 14; i += 2) {
        const float lo = x_min + i * step, hi = lo + 2 * step;
        rail_face(lo,z_min,hi,z_min,-.65f,0,slate,3,{0,0,512,256});
        rail_face(lo,z_max,hi,z_max,-.65f,0,slate,3,{0,0,512,256});
    }
    const float chart_near = rotor_center.z - chart_radius;
    const float chart_far = rotor_center.z + chart_radius;
    for (float edge : {chart_near, chart_far})
        rail_face(-chart_radius,edge,chart_radius,edge,0,chart_height,brass,5,{0,64,512,96});
    for (float edge : {-chart_radius, chart_radius})
        rail_face(edge,chart_near,edge,chart_far,0,chart_height,brass,5,{0,64,512,96});
    for (int i = 0; i < 6; ++i) {
        const float z = -4.6f + i * 2.0f;
        for (float edge : {-3.15f, 3.15f}) {
            rail_face(edge,z-.78f,edge,z+.78f,.12f,.94f,grille,6);
            rail_face(edge,z-.78f,edge,z+.78f,.94f,1.02f,brass,7);
        }
    }
    for (int i = 0; i < 10; ++i) {
        const float x = (i - 5) * 1.3f;
        rail_face(x,8.5f,x+1.3f,8.5f,.12f,.93f,grille,6);
    }

    // Outdoor light motes obey the same opaque depth as the gallery. Sort
    // this small translucent batch so near effects blend over distant ones.
    struct Mote { Vec3 center; float depth, radius; };
    std::array<Mote, 21> motes;
    motes[0] = {rotor_center, camera.to_view(rotor_center).z, .83f + .12f * std::sin(time * 2)};
    for (unsigned i = 1; i < motes.size(); ++i) {
        const float angle = i * turn / (motes.size() - 1) + time * 2;
        const float radius = i < 12 ? 1.16f : 2.40f;
        const Vec3 center{std::cos(angle) * radius,
            2.45f + .86f * std::sin(angle * 2 + time), rotor_center.z + std::sin(angle) * radius};
        motes[i] = {center, camera.to_view(center).z, i % 3 ? .15f : .26f};
    }
    std::sort(motes.begin(), motes.end(), [](const Mote& a, const Mote& b) {
        return a.depth > b.depth;
    });
    for (const auto& mote : motes) {
        auto corner = [&](float x, float y) -> Vec3 {
            const auto c = mote.center, a = camera.right, b = camera.up;
            return {c.x + a.x*x + b.x*y, c.y + a.y*x + b.y*y, c.z + a.z*x + b.z*y};
        };
        const float r = mote.radius;
        draw({corner(-r,r),corner(r,r),corner(r,-r),corner(-r,-r)}, glow, 7);
    }
    return stats;
}
}  // namespace raster_demo
