#include <pxa/app.hpp>
#include <optional>

// Keep the same direct Surface across the half-turn and present exactly once
// after it. Continuous animation or recreating the stream masks a dropped
// first frame when the presenter and panel disagree about direct mode.
struct StaticRotationSurface {
    std::optional<pxa::Surface> surface;
    std::uint64_t first_frame_time = 0;
    unsigned phase = 0;
    std::uint8_t rotation = 0;
    bool initializing = false;
    bool ready = false;

    pxa::Task<void> initialize(pxa::Context& context) {
        auto window = co_await context.window().snapshot(true);
        if (!window) co_return std::unexpected(window.error());
        auto fullscreen = context.window().fullscreen();
        if (!fullscreen) co_return fullscreen;
        pxa::SurfaceOptions options{
            .width = static_cast<std::uint16_t>(window->pixel_width / 2),
            .height = static_cast<std::uint16_t>(window->pixel_height / 2),
            .buffers = 2, .direct_scanout = true};
        auto created = co_await context.surface().create_mapped(options);
        if (!created) co_return std::unexpected(created.error());
        surface.emplace(std::move(*created));
        auto configured = co_await surface->configure({
            .width = options.width, .height = options.height});
        if (!configured) surface.reset();
        ready = configured.has_value();
        co_return configured;
    }

    void on_foreground(pxa::Context& context) {
        if (initializing) return;
        initializing = true;
        (void)context.tasks().start(initialize(context));
    }
    void on_window_changed(const pxa::WindowMetrics& metrics) {
        rotation = metrics.rotation;
    }
    void on_frame(pxa::Context& context, pxa::game::FrameTick tick) {
        if (!ready || !surface || phase == 3) return;
        if (phase == 1) {
            if (tick.timestamp_us - first_frame_time < 1500000) return;
            if (context.window().request_orientation(
                    pxa::OrientationPreference::rotation_180)) phase = 2;
            return;
        }
        if (phase == 2 && rotation != 2) return;
        auto frame = surface->acquire();
        if (!frame) return;
        auto pixels = frame->rgb565();
        if (!pixels) return;
        for (std::uint32_t y = 0; y < pixels->height(); ++y) {
            auto row = *pixels->row(y);
            for (std::uint32_t x = 0; x < pixels->width(); ++x) {
                // Final frame: red/green on top, blue/white on bottom.
                row[x] = phase == 0 ? 0x07ff :
                    y < pixels->height() / 2 ?
                        (x < pixels->width() / 2 ? 0xf800 : 0x07e0) :
                        (x < pixels->width() / 2 ? 0x001f : 0xffff);
            }
        }
        if (frame->present(phase == 0 ? 1 : 2)) {
            if (phase == 0) {
                first_frame_time = tick.timestamp_us;
                phase = 1;
            } else {
                phase = 3;
                (void)context.log().write(pxa::LogLevel::info,
                    "Static rotation: frame 2 submitted once; rotation=2");
            }
        }
    }
};
PXA_GAME(StaticRotationSurface)
