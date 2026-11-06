// SPDX-License-Identifier: Apache-2.0
#include <pxa-ext/peripherals.hpp>
#include <pxa/canvas.hpp>
#include <pxa/ui_component.hpp>
#include <pxa/ui_layout.hpp>
#include <pxa/ui_environment.hpp>
#include <pxa/ui_refresh.hpp>
#include <chrono>
#include <cstdio>
#include <optional>

using namespace pxa::ui;
using namespace pxa::ui::literals;
namespace ports = pxa::ext::peripherals;
using Clock = std::chrono::steady_clock;

struct CameraApp {
    Environment<> environment;
    State<std::string> status{"点击预览或拍照"};
    State<uint32_t> layout_revision{0};
    CanvasRef canvas;
    CanvasCommands<128> commands;
    std::optional<pxa::Asset> image;
    std::array<ports::PortInfo, PXA_PERIPHERALS_MAX_PORTS> available{};
    std::array<std::byte, PXA_VISION_MAX_JPEG> photo{};
    ports::Port port;
    pxa::Permission permission;
    pxa::Context* context = nullptr;
    bool running = false, preview = false, photograph = false, dirty = true;
    uint32_t next_photo = 1, frames = 0;
    Clock::time_point measured_at{};
    int width = 240, height = 150;

    void fail(pxa::Error error) {
        port.reset(); permission.reset();
        running = preview = photograph = false;
        const auto message = "摄像头错误 " + std::to_string(int(error));
        status.set(message);
        (void)context->log().write(pxa::LogLevel::warning, message);
    }

    pxa::Task<void> save(ports::FrameStream frame) {
        size_t received = 0;
        unsigned reads = 0;
        const auto deadline = Clock::now() + std::chrono::seconds(30);
        for (;;) {
            // One extra byte allows EOF to be observed even at the JPEG limit.
            std::byte extra{};
            auto target = received < photo.size() ? std::span(photo).subspan(received)
                                                  : std::span(&extra, 1);
            auto part = frame.read(target);
            if (part) {
                if (!*part) break;
                if (*part > photo.size() - received)
                    co_return std::unexpected(pxa::Error::limit_exceeded);
                received += *part;
            } else if (part.error() != pxa::Error::would_block) {
                co_return std::unexpected(part.error());
            }
            if (Clock::now() > deadline) co_return std::unexpected(pxa::Error::timed_out);
            if (!part || ++reads % 12 == 0) {
                if (auto yielded = co_await context->clock().yield(); !yielded) co_return yielded;
            }
        }
        if (received < 4 || photo[0] != std::byte{0xff} || photo[1] != std::byte{0xd8})
            co_return std::unexpected(pxa::Error::protocol_error);
        char name[48];
        // Probe existing photos so numbering survives application restarts.
        for (;;) {
            if (!next_photo) co_return std::unexpected(pxa::Error::limit_exceeded);
            std::snprintf(name, sizeof(name), "photo-%06u.jpg", unsigned(next_photo++));
            auto existing = co_await context->fs().stat(name);
            if (!existing) {
                if (existing.error() != pxa::Error::not_found) co_return std::unexpected(existing.error());
                break;
            }
        }
        auto file = co_await context->fs().open("capture.tmp", pxa::OpenMode::write |
            pxa::OpenMode::create | pxa::OpenMode::truncate);
        if (!file) co_return std::unexpected(file.error());
        size_t written = 0;
        while (written < received) {
            auto part = file->write(std::span(photo).subspan(written, std::min(size_t(4096), received - written)));
            if (part) {
                if (!*part || *part > received - written) co_return std::unexpected(pxa::Error::io_error);
                written += *part;
            } else if (part.error() != pxa::Error::would_block) {
                co_return std::unexpected(part.error());
            }
            if (Clock::now() > deadline) co_return std::unexpected(pxa::Error::timed_out);
            if (auto yielded = co_await context->clock().yield(); !yielded) co_return yielded;
        }
        if (auto closed = file->close(); !closed) co_return closed;
        auto renamed = co_await context->fs().rename("capture.tmp", name);
        if (!renamed) co_return renamed;
        status.set("已保存 " + std::string(name));
        (void)context->log().write(pxa::LogLevel::info,
            "Camera photo saved: " + std::string(name) + " bytes=" + std::to_string(received));
        auto saved_file = co_await context->fs().open(name, pxa::OpenMode::read);
        if (!saved_file) co_return std::unexpected(saved_file.error());
        auto decoded = co_await context->assets().decode_image(std::move(*saved_file),
            {.max_width=320, .max_height=320, .max_pixels=102400,
             .max_encoded_bytes=PXA_VISION_MAX_JPEG, .formats=pxa::ImageFormats::jpeg});
        if (!decoded) co_return std::unexpected(decoded.error());
        image.emplace(std::move(*decoded)); dirty = true;
        co_return pxa::Result<void>{};
    }

    pxa::Task<void> run() {
        auto timing = co_await context->clock().configure(16000);
        if (!timing) co_return std::unexpected(timing.error());
        auto client = co_await ports::open(*context);
        if (!client) co_return std::unexpected(client.error());
        if (client->version_minor() < 4) co_return std::unexpected(pxa::Error::unsupported);
        auto count = co_await client->list(available);
        if (!count) co_return std::unexpected(count.error());
        size_t selected = 0;
        while (selected < *count && (available[selected].kind != PXA_PERIPHERALS_VISION ||
            available[selected].scope() != "camera.jpeg" || !available[selected].available())) ++selected;
        if (selected == *count) co_return std::unexpected(pxa::Error::unsupported);
        auto grant = co_await context->permissions().acquire(PXA_PERIPHERALS_PERMISSION, "camera.jpeg");
        if (!grant) co_return std::unexpected(grant.error());
        permission = std::move(*grant);
        if (!preview && !photograph) { running = false; permission.reset(); co_return pxa::Result<void>{}; }
        auto opened = co_await client->open_port(available[selected], PXA_PERIPHERALS_MODE_STREAM, 0, 0, permission);
        if (!opened) co_return std::unexpected(opened.error());
        port = std::move(*opened);
        status.set("预览中");
        measured_at = Clock::now(); frames = 0;
        while (preview || photograph) {
            auto frame = co_await client->frame(port);
            if (!frame) co_return std::unexpected(frame.error());
            if (photograph) {
                photograph = false;
                status.set("正在保存照片");
                auto saved = co_await save(std::move(*frame));
                if (!saved) co_return saved;
            } else {
                auto decoded = co_await context->assets().decode_image(std::move(*frame),
                    {.max_width=320, .max_height=320, .max_pixels=102400,
                     .max_encoded_bytes=PXA_VISION_MAX_JPEG, .formats=pxa::ImageFormats::jpeg});
                if (!decoded) co_return std::unexpected(decoded.error());
                image.emplace(std::move(*decoded));
                dirty = true;
                ++frames;
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - measured_at).count();
                if (elapsed >= 5000) {
                    char text[128];
                    std::snprintf(text, sizeof(text), "Camera preview: frames=%u elapsed_ms=%lld fps_x100=%lld",
                        unsigned(frames), static_cast<long long>(elapsed), static_cast<long long>(frames * 100000ull / elapsed));
                    (void)context->log().write(pxa::LogLevel::info, text);
                    measured_at = Clock::now(); frames = 0;
                }
            }
            if (auto yielded = co_await context->clock().yield(); !yielded) co_return yielded;
        }
        port.reset(); permission.reset(); running = false;
        co_return pxa::Result<void>{};
    }

    void start(bool take_photo) {
        if (take_photo) photograph = true;
        else preview = true;
        if (running) return;
        running = true; status.set("正在打开摄像头");
        if (auto task = context->foreground_tasks().start(run()); !task) fail(task.error());
    }
    void stop() {
        preview = photograph = false;
        status.set("已停止预览");
    }
    void paint() {
        if (!dirty || !canvas.mounted()) return;
        commands.reset(); commands.rect(0, 0, width, height, 0x10151aff);
        if (image) commands.image({0, 0, width, height}, *image, ImageFit::contain);
        if (auto shown = canvas.present(commands); !shown) { fail(shown.error()); return; }
        dirty = false;
    }
    pxa::Result<void> on_start(pxa::Context& ctx, std::span<const std::byte> config) {
        context = &ctx;
        ctx.foreground_tasks().on_error(this, [](void* self, pxa::Error error) noexcept {
            static_cast<CameraApp*>(self)->fail(error);
        });
        auto initialized = environment.initialize(config);
        if (initialized) layout_revision.set(layout_revision.get() + 1);
        return initialized;
    }
    void on_foreground(pxa::Context& ctx) {
        (void)ctx.window().fullscreen(pxa::WindowBarMode::hidden, pxa::WindowBarMode::hidden);
        dirty = true;
    }
    pxa::Result<bool> on_event(pxa::Context&, const pxa::Event& event) {
        auto result = environment.update(event);
        if (result && *result) layout_revision.set(layout_revision.get() + 1);
        paint(); return result;
    }
    void on_background(pxa::Context&) {
        port.reset(); permission.reset(); running = preview = photograph = false;
        status.set("已暂停，摄像头已释放");
    }
    void on_stop(pxa::StopReason) { port.reset(); permission.reset(); image.reset(); }
    auto content() {
        const auto m = environment.display().get();
        const auto density = std::max(uint32_t(1), m.density_q16);
        const auto safe = safe_rectangle(m);
        const int logical_width = std::max(80, int((uint64_t(std::max(1, safe.width)) * 65536) / density) - 16);
        const int available_height = int((uint64_t(std::max(1, safe.height)) * 65536) / density);
        const int logical_height = std::clamp(available_height - 102, 48, logical_width * 3 / 4);
        width = logical_width;
        height = logical_height;
        dirty = true;
        return Scroll(Column(
            Text("相机").font(Font::caption),
            Canvas(canvas, logical_width, logical_height),
            Text(status).font(Font::caption).fill(),
            Row(Button("预览").on_click([this] { start(false); }),
                Button("拍照").on_click([this] { start(true); }),
                Button("停止").on_click([this] { stop(); })).gap(4_dp).fill()
        ).gap(4_dp).padding(8_dp).fill()).fill();
    }
    auto view() {
        return SafeArea(environment.display(), Refresh(layout_revision, [this] { return content(); }));
    }
};
PXA_APPLICATION(CameraApp)
