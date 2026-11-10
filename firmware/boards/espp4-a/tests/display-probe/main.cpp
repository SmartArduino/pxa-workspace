// SPDX-License-Identifier: Apache-2.0
#include <pxa/app.hpp>
#include <cstdio>
#include <optional>

// A bounded native GameRender workload. Tap to cycle 0/90/180/270 degrees.
// Compare identical packages/scenes and completed panel frames, not submissions.
struct DisplayProbe {
    std::optional<pxa::game::Renderer> renderer;
    pxa::game::DrawBuffer<512> commands;
    unsigned rotation = 0, frames = 0;
    bool loading = false;

    pxa::Task<void> initialize(pxa::Context& context) {
        auto window = co_await context.window().snapshot(true);
        if (!window) { loading = false; co_return std::unexpected(window.error()); }
        if (window->rotation != rotation) { loading = false; co_return pxa::Result<void>{}; }
        pxa::game::RenderOptions options;
        options.width = static_cast<uint16_t>(window->pixel_width / 2);
        options.height = static_cast<uint16_t>(window->pixel_height / 2);
        options.scratch = pxa::game::Scratch::none;
        auto created = co_await context.game().create(options);
        loading = false;
        if (!created) co_return std::unexpected(created.error());
        renderer.emplace(std::move(*created));
        char text[80];
        std::snprintf(text, sizeof(text), "P4 probe ready rotation=%u render=%ux%u",
            rotation, unsigned(options.width), unsigned(options.height));
        (void)context.log().write(pxa::LogLevel::info, text);
        co_return pxa::Result<void>{};
    }
    pxa::Result<void> on_start(pxa::Context& context, std::span<const std::byte>) {
        return context.window().request_orientation(pxa::OrientationPreference::rotation_0);
    }
    void on_update(pxa::Context& context, uint32_t) {
        if (!renderer && !loading) {
            loading = true;
            if (!context.foreground_tasks().start(initialize(context))) loading = false;
        }
    }
    void on_pointer(pxa::Context& context, const pxa::input::Pointer& pointer) {
        if (pointer.action() != pxa::input::PointerPhase::down || loading) return;
        renderer.reset(); rotation = (rotation + 1) & 3; frames = 0;
        (void)context.window().request_orientation(static_cast<pxa::OrientationPreference>(
            unsigned(pxa::OrientationPreference::rotation_0) + rotation));
    }
    void on_frame(pxa::Context&, pxa::game::FrameTick) {
        if (!renderer) return;
        const int width = renderer->info().render_width, height = renderer->info().render_height;
        auto frame = renderer->frame(commands);
        frame.clear({0x0841});
        auto rectangle = [&](int x, int y, int w, int h, uint16_t color) {
            const auto l = int16_t(x * 16), r = int16_t((x + w) * 16);
            const auto t = int16_t(y * 16), b = int16_t((y + h) * 16);
            frame.quad({l,t,r,t,r,b,l,b}, {color});
        };
        rectangle(0,0,width/2,height/2,0xf800);
        rectangle(width/2,0,width-width/2,height/2,0x07e0);
        rectangle(0,height/2,width/2,height-height/2,0x001f);
        rectangle(width/2,height/2,width-width/2,height-height/2,0xffff);
        rectangle(int(frames % unsigned(width-16)),height/2-4,16,8,0xffe0);
        if (frame.submit()) ++frames;
    }
};
PXA_GAME(DisplayProbe)
