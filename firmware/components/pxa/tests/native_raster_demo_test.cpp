// SPDX-License-Identifier: MIT
#include "guests/native-raster-demo/scene.hpp"

#include <pxa/raster.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>

#define DEMO_LOG(...) do { std::printf(__VA_ARGS__); std::puts(""); } while (false)

#ifdef PXA_NATIVE_DEMO_REFERENCE
extern "C" void reference_execute_rows(
    const std::uint8_t*, const pxa_raster_draw_list_view_t*, const pxa_raster_target_t*,
    const pxa_raster_resources_t*, std::uint16_t, std::uint16_t, pxa_raster_telemetry_t*);
#endif

namespace {
constexpr unsigned width = 480, height = 480, split = 317;
using Plane = std::array<std::uint16_t, width * height>;
struct alignas(64) Workspace {
    Plane color, depth;
    Plane whole_color, whole_depth;
    pxa::game::DrawBuffer<pxa::game::limits::draw_list_bytes> commands;
};

std::span<const std::uint8_t> packet;

std::uint32_t hash(const Plane& data) {
    std::uint32_t value = 2166136261u;
    for (const auto pixel : data) value = (value ^ pixel) * 16777619u;
    return value;
}

std::uint64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void require(bool condition, const char* message, int frame) {
    if (condition) return;
    DEMO_LOG("validation_failed frame=%d reason=%s", frame, message);
    std::abort();
}

void capture(const char* directory, int index, const Plane& image) {
    if (!directory) return;
    char path[1024];
    std::snprintf(path, sizeof path, "%s/shot-%d.ppm", directory, index);
    auto* file = std::fopen(path, "wb");
    require(file != nullptr, "open screenshot", index);
    std::fprintf(file, "P6\n%u %u\n255\n", width, height);
    for (const std::uint16_t pixel : image) {
        const std::uint8_t rgb[3] = {static_cast<std::uint8_t>((pixel >> 11) * 255 / 31),
            static_cast<std::uint8_t>(((pixel >> 5) & 63) * 255 / 63),
            static_cast<std::uint8_t>((pixel & 31) * 255 / 31)};
        std::fwrite(rgb, 1, 3, file);
    }
    require(std::fclose(file) == 0, "write screenshot", index);
}

void save_binary(const char* directory, const char* name, const void* data, std::size_t size) {
    char path[1024];
    std::snprintf(path, sizeof path, "%s/%s", directory, name);
    auto* file = std::fopen(path, "wb");
    require(file != nullptr, "open binary", -1);
    require(std::fwrite(data, 1, size, file) == size, "write binary", -1);
    require(std::fclose(file) == 0, "close binary", -1);
}

int run_demo(const char* captures) {
    auto workspace = std::make_unique<Workspace>();
    constexpr int samples = 3;
    auto palette = raster_demo::make_palette();
    std::array<raster_demo::Texture, raster_demo::texture_count> textures;
    pxa_raster_resources_t resources{};
    resources.capabilities = PXA_RASTER_CAP_KNOWN_MASK;
    resources.palette = palette.data();
    resources.palette_light_levels = raster_demo::light_levels;
    for (unsigned i = 0; i < textures.size(); ++i) {
        textures[i] = raster_demo::make_texture(i);
        resources.textures[i] = {reinterpret_cast<const std::uint8_t*>(textures[i].data()),
            raster_demo::texture_size, raster_demo::texture_size};
    }
    if (captures) {
        save_binary(captures, "palette.rgb565", palette.data(), sizeof(palette));
        save_binary(captures, "textures.index8", textures.data(), sizeof(textures));
    }
    const pxa_raster_target_t target{
        .pixels = workspace->color.data(), .depth_pixels = workspace->depth.data(),
        .stride_pixels = width, .depth_stride_pixels = width, .width = width, .height = height,
        .prefilled_commands = 0, .scratch_mode = PXA_RASTER_SCRATCH_DEPTH16,
    };
    pxa::Transport transport;
    pxa::game::Renderer renderer(transport, 1, resources.capabilities);
    const auto projector = raster_demo::Projector::create(width, height, 1.1f, .25f, 44.f);
    require(projector.has_value(), "projection", -1);
    DEMO_LOG("begin %ux%u frames=%d samples=%d", width, height, raster_demo::scene_frames, samples);
    for (int index = 0; index < raster_demo::scene_frames; ++index) {
        auto frame = renderer.frame(workspace->commands);
        const auto stats = raster_demo::draw_scene(frame, *projector, index);
        require(frame.submit().has_value(), "serialize", index);
        require(stats.source_faces == 778 && stats.projected_faces >= 100 &&
                stats.blended_faces >= 1 && stats.cutout_faces > stats.blended_faces,
                "scene coverage", index);
        pxa_raster_draw_list_view_t view{};
        require(pxa_raster_validate_draw_list(packet.data(), packet.size(), &target,
                &resources, &view) == PXA_STATUS_OK && view.uses_depth && view.uses_palette,
                "validate", index);
        pxa_raster_telemetry_t whole{}, bands{};
        pxa_raster_execute_draw_list(packet.data(), &view, &target, &resources, &whole);
        workspace->whole_color = workspace->color;
        workspace->whole_depth = workspace->depth;
        const auto color = hash(workspace->color), depth = hash(workspace->depth);
        auto render = [&](decltype(&pxa_raster_execute_draw_list_rows) rows, pxa_raster_telemetry_t* stats) {
            rows(packet.data(), &view, &target, &resources, 0, split, stats);
            rows(packet.data(), &view, &target, &resources, split, height, stats);
        };
        render(pxa_raster_execute_draw_list_rows, &bands);
        require(workspace->whole_color == workspace->color && workspace->whole_depth == workspace->depth &&
                whole.covered_pixels == bands.covered_pixels, "split raster", index);
#ifdef PXA_NATIVE_DEMO_REFERENCE
        pxa_raster_telemetry_t reference{};
        render(reference_execute_rows, &reference);
        require(workspace->whole_color == workspace->color && workspace->whole_depth == workspace->depth &&
                whole.covered_pixels == reference.covered_pixels, "baseline raster", index);
#endif
        std::array<unsigned, samples> timings{}, reference_timings{};
        auto measure = [&](decltype(&pxa_raster_execute_draw_list_rows) rows) {
            const auto started = now_us();
            render(rows, nullptr);
            return static_cast<unsigned>(now_us() - started);
        };
        for (int sample = 0; sample < samples; ++sample) {
#ifdef PXA_NATIVE_DEMO_REFERENCE
            if ((sample + index) % 2 == 0)
                reference_timings[sample] = measure(reference_execute_rows);
#endif
            timings[sample] = measure(pxa_raster_execute_draw_list_rows);
#ifdef PXA_NATIVE_DEMO_REFERENCE
            if ((sample + index) % 2 != 0)
                reference_timings[sample] = measure(reference_execute_rows);
#endif
        }
        std::sort(timings.begin(), timings.end());
        std::sort(reference_timings.begin(), reference_timings.end());
        DEMO_LOG("frame=%d shot=%d faces=%u cutout=%u blended=%u commands=%u bytes=%u "
                 "covered=%llu color=%08x depth=%08x before_us=%u after_us=%u",
                 index, index / raster_demo::frames_per_shot, stats.projected_faces,
                 stats.cutout_faces, stats.blended_faces, view.command_count, view.total_size,
                 static_cast<unsigned long long>(whole.covered_pixels), color, depth,
                 reference_timings[samples/2], timings[samples/2]);
        if (captures && index % 10 == 0) {
            char name[24];
            std::snprintf(name, sizeof name, "frame-%03d.pxrl", index);
            save_binary(captures, name, packet.data(), packet.size());
        }
        if (index % raster_demo::frames_per_shot == raster_demo::frames_per_shot / 2)
            capture(captures, index / raster_demo::frames_per_shot, workspace->color);
    }
    DEMO_LOG("complete");
    return 0;
}
}  // namespace

pxa::Result<std::uint32_t> pxa::Transport::io(
    std::uint64_t handle, std::uint32_t operation, std::span<std::byte> data) noexcept {
    if (handle != 1 || operation != 0x101) return std::unexpected(pxa::Error::invalid_argument);
    packet = {reinterpret_cast<const std::uint8_t*>(data.data()), data.size()};
    return data.size();
}
pxa::Result<void> pxa::Transport::close(std::uint64_t) noexcept { return {}; }

int main(int argc, char** argv) { return run_demo(argc == 2 ? argv[1] : nullptr); }
