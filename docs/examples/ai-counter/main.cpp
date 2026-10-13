#include <pxa/app.hpp>
#include <pxa/ui_component.hpp>
#include <pxa/ui_environment.hpp>
#include <pxa/ui_layout.hpp>
#include <limits>

using namespace pxa;
using namespace pxa::ui;
using namespace pxa::ui::literals;

struct Counter {
    Environment<> environment;
    State<int> count{0};

    Result<void> on_start(Context&, std::span<const std::byte> configuration) {
        return environment.initialize(configuration);
    }
    Result<bool> on_event(Context&, const Event& event) {
        return environment.update(event);
    }
    auto view() {
        return SafeArea(environment.display(), Column(
            Text("计数器").font(Font::title),
            Text(count).font(Font::headline).text_align(TextAlign::center),
            Row(
                Button("加一").on_click([this] {
                    count.update([](int value) {
                        return value < std::numeric_limits<int>::max() ? value + 1 : value;
                    });
                }).height(44_dp).grow(),
                Button("归零").on_click([this] {
                    count.set(0);
                }).height(44_dp).grow()
            ).gap(8_dp).fill().max_width(320_dp)
        ).gap(8_dp).fill().fill_height().align(Align::center)
          .justify(Justify::center)).padding(8_dp);
    }
};

PXA_APPLICATION(Counter)
