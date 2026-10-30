// SPDX-License-Identifier: Apache-2.0
#include <pxa-ext/peripherals.hpp>
#include <pxa/ui_component.hpp>
#include <pxa/ui_layout.hpp>
#include <pxa/ui_environment.hpp>
using namespace pxa::ui;
using namespace pxa::ui::literals;
namespace ports = pxa::ext::peripherals;

struct IndicatorApp {
    Environment<> environment;
    State<std::string> status{"Discovering indicator"};
    State<bool> rgb{true};
    std::array<ports::PortInfo, PXA_PERIPHERALS_MAX_PORTS> available{};
    ports::Port port;
    pxa::Permission permission;
    pxa::Context* context = nullptr;
    bool starting = false;
    bool request_enabled = true;
    bool monochrome = false;
    pxa::Task<void> open(pxa::Context& context) {
        auto client = co_await ports::open(context);
        if (!client) { status.set("Indicator extension unavailable"); starting = false; co_return pxa::Result<void>{}; }
        auto count = co_await client->list(available);
        if (!count) { status.set("Discovery failed"); starting = false; co_return pxa::Result<void>{}; }
        for (size_t i = 0; i < *count; ++i) {
            const bool is_rgb = available[i].kind == PXA_PERIPHERALS_RGB && available[i].scope() == "indicator.rgb";
            const bool is_binary = available[i].kind == PXA_PERIPHERALS_GPIO && available[i].scope() == "indicator.status";
            if ((!is_rgb && !is_binary) || !available[i].available()) continue;
            monochrome = is_binary; rgb.set(is_rgb);
            auto grant = co_await context.permissions().acquire(PXA_PERIPHERALS_PERMISSION, available[i].scope());
            if (!grant) { status.set("Indicator permission denied; use Connect to retry"); request_enabled = false; starting = false; co_return pxa::Result<void>{}; }
            permission = std::move(*grant);
            auto resource = co_await client->open_port(available[i], is_rgb ? PXA_PERIPHERALS_MODE_RGB : PXA_PERIPHERALS_MODE_OUTPUT, 0, 0, permission);
            if (resource) { port = std::move(*resource); status.set("Ready"); }
            else { permission.reset(); status.set("Indicator busy or unavailable"); }
            starting = false; co_return pxa::Result<void>{};
        }
        status.set("This board has no supported indicator"); starting = false;
        co_return pxa::Result<void>{};
    }
    void begin(pxa::Context& context) {
        if (starting || port || !request_enabled) return;
        starting = true;
        if (!context.foreground_tasks().start(open(context))) { starting = false; status.set("Task limit"); }
    }
    pxa::Result<void> on_start(pxa::Context& ctx, std::span<const std::byte> config) {
        context = &ctx; return environment.initialize(config);
    }
    pxa::Result<bool> on_event(pxa::Context&, const pxa::Event& event) { return environment.update(event); }
    void on_foreground(pxa::Context& context) { begin(context); }
    void on_background(pxa::Context&) { port.reset(); permission.reset(); starting = false; status.set("Paused; indicator released"); }
    void on_stop(pxa::StopReason) { port.reset(); permission.reset(); }
    void color(uint8_t r, uint8_t g, uint8_t b) {
        if (!port) return;
        const std::array value{std::byte((r || g || b) ? 1 : 0)};
        auto result = monochrome ? port.write(value) : port.rgb(r, g, b);
        status.set(result ? "Color accepted" : "Indicator busy or unavailable");
        (void)context->log().write(pxa::LogLevel::info, result ? "Indicator write accepted" : "Indicator write failed");
    }
    auto view() {
        return SafeArea(environment.display(), Scroll(Column(Text("Board indicator").font(Font::caption),
            Button("Connect").on_click([this]{request_enabled = true; begin(*context);}),
            When(rgb, [this]{ return Row(Button("Red").on_click([this]{color(24,0,0);}),
                Button("Green").on_click([this]{color(0,24,0);}), Button("Blue").on_click([this]{color(0,0,24);})).gap(8_dp); },
                []{return Text("On / off indicator");}),
            Row(Button("On").on_click([this]{color(24,24,24);}), Button("Off").on_click([this]{color(0,0,0);})).gap(8_dp),
            Text(status).font(Font::caption).fill()
        ).gap(8_dp).padding(8_dp).fill()).fill());
    }
};
PXA_APPLICATION(IndicatorApp)
