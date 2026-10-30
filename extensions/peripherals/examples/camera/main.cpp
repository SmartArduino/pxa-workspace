// SPDX-License-Identifier: Apache-2.0
#include <pxa-ext/vision.hpp>
#include <pxa/ui_component.hpp>
#include <pxa/ui_layout.hpp>
#include <pxa/ui_environment.hpp>
#include <chrono>
using namespace pxa::ui;
using namespace pxa::ui::literals;
namespace ports = pxa::ext::peripherals;
struct CameraApp {
    Environment<> environment;
    State<std::string> status{"Choose Capture to request camera permission"};
    std::array<ports::PortInfo, PXA_PERIPHERALS_MAX_PORTS> available{};
    std::array<std::byte, PXA_VISION_MAX_JPEG> bytes{};
    ports::Port port;
    pxa::Permission permission;
    ports::FrameReader reader{PXA_VISION_JPEG};
    pxa::Context* context = nullptr;
    ports::FrameInfo info{};
    std::chrono::steady_clock::time_point deadline;
    size_t received = 0;
    bool busy = false, header_ready = false;
    uint16_t format = PXA_VISION_JPEG;
    void fail(std::string_view message, pxa::Error error) {
        port.reset(); permission.reset(); busy = false;
        auto text = std::string(message) + " (" + std::to_string(static_cast<int>(error)) + ")";
        status.set(text);
        (void)context->log().write(pxa::LogLevel::warning, text);
    }
    pxa::Task<void> open() {
        auto timing = co_await context->clock().configure(16000);
        if (!timing) { fail("Clock unavailable", timing.error()); co_return pxa::Result<void>{}; }
        auto client = co_await ports::open(*context);
        if (!client) { fail("Vision extension unavailable", client.error()); co_return pxa::Result<void>{}; }
        if (client->version_minor() < 3) { fail("Vision needs peripherals 0.3", pxa::Error::unsupported); co_return pxa::Result<void>{}; }
        auto count = co_await client->list(available);
        if (!count) { fail("Discovery failed", count.error()); co_return pxa::Result<void>{}; }
        const auto scope = format == PXA_VISION_JPEG ? "camera.jpeg" : "vision.inference";
        for (size_t i = 0; i < *count; ++i) {
            if (available[i].kind != PXA_PERIPHERALS_VISION || available[i].scope() != scope || !available[i].available()) continue;
            auto grant = co_await context->permissions().acquire(PXA_PERIPHERALS_PERMISSION, scope);
            if (!grant) { fail("Permission denied", grant.error()); co_return pxa::Result<void>{}; }
            permission = std::move(*grant);
            auto opened = co_await client->open_port(available[i], PXA_PERIPHERALS_MODE_STREAM, 0, 0, permission);
            if (!opened) { fail("Camera unavailable", opened.error()); co_return pxa::Result<void>{}; }
            port = std::move(*opened);
            deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
            status.set("Reading frame...");
            co_return pxa::Result<void>{};
        }
        fail("This board has no selected vision port", pxa::Error::unsupported);
        co_return pxa::Result<void>{};
    }
    pxa::Task<void> save() {
        const auto name = format == PXA_VISION_JPEG ? "snapshot.jpg" : "inference.json";
        auto file = co_await context->fs().open("capture.tmp", pxa::OpenMode::write | pxa::OpenMode::create | pxa::OpenMode::truncate);
        if (!file) { fail("Cannot open private capture file", file.error()); co_return pxa::Result<void>{}; }
        size_t offset = 0;
        while (offset < received) {
            auto written = file->write(std::span(bytes).subspan(offset, std::min(size_t(4096), received - offset)));
            if (written) {
                if (!*written || *written > received - offset) { fail("Invalid file write", pxa::Error::protocol_error); co_return pxa::Result<void>{}; }
                offset += *written;
            } else if (written.error() != pxa::Error::would_block) {
                fail("Cannot save capture", written.error()); co_return pxa::Result<void>{};
            }
            if (std::chrono::steady_clock::now() > deadline) { fail("Save deadline expired", pxa::Error::timed_out); co_return pxa::Result<void>{}; }
            if (auto yielded = co_await context->clock().yield(); !yielded) {
                fail("Save interrupted", yielded.error()); co_return pxa::Result<void>{};
            }
        }
        auto closed = file->close();
        if (!closed) { fail("File close failed", closed.error()); co_return pxa::Result<void>{}; }
        auto renamed = co_await context->fs().rename("capture.tmp", name);
        if (!renamed) { fail("File commit failed", renamed.error()); co_return pxa::Result<void>{}; }
        permission.reset(); busy = false;
        status.set("Saved " + std::string(name) + " (" + std::to_string(received) + " bytes) in app private files");
        (void)context->log().write(pxa::LogLevel::info, "Vision frame saved: " + std::string(name) + " bytes=" + std::to_string(received));
        co_return pxa::Result<void>{};
    }
    void capture(uint16_t selected) {
        if (busy) return;
        busy = true; format = selected; reader = ports::FrameReader(format);
        received = 0; header_ready = false; info = {};
        status.set("Opening selected camera port...");
        if (!context->foreground_tasks().start(open())) fail("Task limit", pxa::Error::resource_limit);
    }
    void pump() {
        if (!port) return;
        if (std::chrono::steady_clock::now() > deadline) { fail("Capture deadline expired", pxa::Error::timed_out); return; }
        if (!header_ready) {
            auto frame = reader.header(port);
            if (!frame) { if (frame.error() != pxa::Error::would_block) fail("Frame header failed", frame.error()); return; }
            info = *frame; header_ready = true;
        }
        // Bound Guest work in every event; reads never spin on WouldBlock.
        for (unsigned i = 0; i < 12 && !reader.complete(); ++i) {
            auto part = reader.read_payload(port, std::span(bytes).subspan(received, info.size - received));
            if (!part) { if (part.error() != pxa::Error::would_block) fail("Frame read failed", part.error()); return; }
            if (!*part) return;
            received += *part;
        }
        if (reader.complete()) {
            port.reset();
            deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            status.set("Saving capture...");
            if (!context->foreground_tasks().start(save())) fail("Task limit", pxa::Error::resource_limit);
        }
    }
    pxa::Result<void> on_start(pxa::Context& ctx, std::span<const std::byte> config) {
        context = &ctx; return environment.initialize(config);
    }
    void on_foreground(pxa::Context& ctx) {
        (void)ctx.window().fullscreen(pxa::WindowBarMode::hidden, pxa::WindowBarMode::hidden);
    }
    pxa::Result<bool> on_event(pxa::Context&, const pxa::Event& event) {
        auto result = environment.update(event); pump(); return result;
    }
    void on_background(pxa::Context&) { port.reset(); permission.reset(); busy = false; status.set("Paused; camera released"); }
    void on_stop(pxa::StopReason) { port.reset(); permission.reset(); }
    auto view() {
        return SafeArea(environment.display(), Scroll(Column(Text("Board camera").font(Font::caption),
            Button("Capture JPEG").on_click([this]{capture(PXA_VISION_JPEG);}),
            Button("Read inference").on_click([this]{capture(PXA_VISION_JSON);}),
            Text(status).font(Font::caption).fill(),
            Text("Local only; no model changes.").font(Font::caption).fill()
        ).gap(4_dp).padding(8_dp).fill()).fill());
    }
};
PXA_APPLICATION(CameraApp)
