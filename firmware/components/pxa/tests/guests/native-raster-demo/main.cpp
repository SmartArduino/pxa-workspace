// SPDX-License-Identifier: MIT
#include <pxa/app.hpp>
#include <pxa/game_upload.hpp>

#include "scene.hpp"

#include <cstdio>
#include <optional>

namespace {
class Demo {
public:
    pxa::Result<void> on_start(pxa::Context& context, std::span<const std::byte>) {
        auto started = context.tasks().start(prepare(context));
        if (!started) return std::unexpected(started.error());
        return {};
    }

    void on_frame(pxa::Context& context, pxa::game::FrameTick) {
        if (!renderer_ || failed_) return;
        auto frame = renderer_->frame(commands_);
        const auto stats = raster_demo::draw_scene(frame, *projector_, frame_number_);
        const auto result = frame.submit();
        if (!result) {
            if (result.error() != pxa::Error::would_block) on_error(context, result.error());
            return;
        }
        if (frame_number_ % 30 == 0) {
            char message[192];
            const auto telemetry = renderer_->telemetry();
            std::snprintf(message, sizeof message,
                "NATIVE3D %ux%u shot=%d frame=%d source=%u projected=%u cutout=%u blended=%u bytes=%u raster_us=%u",
                width_, height_, frame_number_ / raster_demo::frames_per_shot, frame_number_,
                stats.source_faces, stats.projected_faces, stats.cutout_faces, stats.blended_faces,
                static_cast<unsigned>(frame.bytes_used()),
                telemetry ? telemetry->last_host_raster_us : 0);
            (void)context.log().write(pxa::LogLevel::info, message);
        }
        frame_number_ = (frame_number_ + 1) % raster_demo::scene_frames;
    }

    void on_error(pxa::Context& context, pxa::Error error) {
        failed_ = true;
        char message[80];
        std::snprintf(message, sizeof message, "NATIVE3D error=%d frame=%d",
                      static_cast<int>(error), frame_number_);
        (void)context.log().write(pxa::LogLevel::error, message);
    }

private:
    pxa::Task<void> prepare(pxa::Context& context) {
        const auto display = co_await context.window().snapshot();
        if (!display) co_return std::unexpected(display.error());
        width_ = display->pixel_width;
        height_ = display->pixel_height;
        pxa::game::RenderOptions options;
        options.width = width_;
        options.height = height_;
        options.scratch = pxa::game::Scratch::depth16;
        options.direct_scanout = true;
        options.borrow_commands = true;
        options.max_draw_bytes = pxa::game::limits::draw_list_bytes;
        auto renderer = co_await context.game().create(options);
        if (!renderer) co_return std::unexpected(renderer.error());
        auto projector = raster_demo::Projector::create(width_, height_, 1.1f, .25f, 44.f);
        if (!projector) co_return std::unexpected(projector.error());

        pxa::game::Upload upload(*renderer, upload_);
        const auto palette = raster_demo::make_palette();
        if (auto result = upload.palette(palette, raster_demo::light_levels); !result)
            co_return std::unexpected(result.error());
        for (std::uint8_t material = 0; material < raster_demo::texture_count; ++material) {
            const auto texture = raster_demo::make_texture(material);
            if (auto result = upload.texture({material}, raster_demo::texture_size,
                                             raster_demo::texture_size, texture); !result)
                co_return std::unexpected(result.error());
        }
        renderer_.emplace(std::move(*renderer));
        projector_.emplace(*projector);
        (void)context.window().fullscreen(pxa::WindowBarMode::hidden);
        (void)context.window().keep_screen_on(true);
        co_return pxa::Result<void>{};
    }

    std::optional<pxa::game::Renderer> renderer_;
    std::optional<raster_demo::Projector> projector_;
    pxa::game::DrawBuffer<pxa::game::limits::draw_list_bytes> commands_;
    std::array<std::byte, pxa::game::Upload::header_bytes + sizeof(raster_demo::Palette)> upload_{};
    std::uint16_t width_ = 0, height_ = 0;
    int frame_number_ = 0;
    bool failed_ = false;
};
}  // namespace

PXA_APPLICATION(Demo)
