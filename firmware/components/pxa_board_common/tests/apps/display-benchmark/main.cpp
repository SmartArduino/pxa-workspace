#include <pxa/app.hpp>

#include <algorithm>
#include <charconv>
#include <optional>
#include <string_view>
#include <utility>

// Submit a lightweight pattern; PERF START display counts completed LCD frames.
struct DisplayBenchmark {
    static constexpr pxa::game::LoopOptions loop_options{.frame_period_ms = 16};
    std::optional<pxa::Surface> surface;
    std::uint64_t next_frame = 1;
    std::uint16_t offset = 0;
    bool initialized[2]{};
    std::uint16_t previous[2]{};
    bool reported_error = false;
    bool initializing = false;

    void report(pxa::Context& context, std::string_view stage,
                pxa::Error error) {
        if (reported_error) return;
        reported_error = true;
        char message[64] = "Surface ";
        auto* number = std::copy(stage.begin(), stage.end(), message + 8);
        *number++ = ':';
        *number++ = ' ';
        auto [end, status] = std::to_chars(
            number, message + sizeof(message),
            static_cast<int>(error));
        if (status == std::errc{})
            (void)context.log().write(pxa::LogLevel::error,
                {message, static_cast<std::size_t>(end - message)});
    }

    pxa::Task<void> initialize(pxa::Context& context) {
        auto window = co_await context.window().snapshot();
        if (!window) {
            report(context, "window", window.error());
            initializing = false;
            co_return std::unexpected(window.error());
        }
        if (window->pixel_width < 80 || window->pixel_height < 80) {
            initializing = false;
            co_return std::unexpected(pxa::Error::unsupported);
        }
        pxa::SurfaceOptions options{
            .width = static_cast<std::uint16_t>(
                std::min(window->pixel_width, 320u)),
            .height = static_cast<std::uint16_t>(
                std::min(window->pixel_height, 240u)),
            .buffers = 2, .direct_scanout = true};
        auto created = co_await context.surface().create_mapped(options);
        if (!created) {
            report(context, "create", created.error());
            initializing = false;
            co_return std::unexpected(created.error());
        }
        surface.emplace(std::move(*created));
        auto configured = co_await surface->configure({
            .width = options.width, .height = options.height});
        if (!configured) {
            report(context, "configure", configured.error());
            surface.reset();
        }
        initializing = false;
        co_return configured;
    }

    void on_foreground(pxa::Context& context) {
        if (surface || initializing) return;
        initializing = true;
#ifdef PXA_DISPLAY_BENCHMARK_SYSTEM_BARS
        (void)context.window().configure({.edge_to_edge = true,
            .status_bar_mode = pxa::WindowBarMode::visible,
            .navigation_bar_mode = pxa::WindowBarMode::transient});
#else
        (void)context.window().fullscreen();
#endif
        reported_error = false;
        auto started = context.tasks().start(initialize(context));
        if (!started) {
            initializing = false;
            report(context, "task", started.error());
        }
    }

    void on_frame(pxa::Context& context, pxa::game::FrameTick) {
        if (!surface || !*surface) return;
        auto acquired = surface->acquire();
        if (!acquired) {
            if (acquired.error() != pxa::Error::would_block)
                report(context, "acquire", acquired.error());
            return;
        }
        auto pixels = acquired->rgb565();
        if (!pixels) {
            report(context, "pixels", pixels.error());
            return;
        }
        const auto width = pixels->width();
        const auto height = pixels->height();
        const auto index = acquired->buffer_index();
        if (index >= 2) return;
        if (!initialized[index]) {
            for (std::uint32_t y = 0; y < height; ++y) {
                auto row = *pixels->row(y);
                for (std::uint32_t x = 0; x < width; ++x) {
                    std::uint16_t color = (x / 16) % 2 ? 0xffff : 0;
                    if (y < 24 && x < 24) color = 0xf800;
                    else if (y < 24 && x >= width - 24) color = 0x07e0;
                    else if (y >= height - 24 && x < 24) color = 0x001f;
                    else if (y >= height - 24 && x >= width - 24) color = 0xffe0;
                    row[x] = color;
                }
            }
            initialized[index] = true;
        }
        // Touch only two narrow strips; guest rendering must not cap LCD fps.
        // Both leased buffers retain their own previous bar position.
        for (std::uint32_t y = 32; y + 32 < height; ++y) {
            auto row = *pixels->row(y);
            for (std::uint32_t dx = 0; dx < 8; ++dx) {
                const auto old_x = (previous[index] + dx) % width;
                row[old_x] = (old_x / 16) % 2 ? 0xffff : 0;
            }
            for (std::uint32_t dx = 0; dx < 8; ++dx)
                row[(offset + dx) % width] = 0xf81f;
        }
        previous[index] = offset;
        auto presented = acquired->present(next_frame);
        if (presented) {
            ++next_frame;
            offset = static_cast<std::uint16_t>((offset + 1) % width);
        } else {
            report(context, "present", presented.error());
        }
    }
};

PXA_GAME(DisplayBenchmark)
