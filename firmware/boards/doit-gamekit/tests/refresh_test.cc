#include "doit_gamekit_refresh.h"
#include <lvgl.h>
#include <cassert>
#include <cstdio>

using namespace doit_gamekit;
struct State {
    RefreshTracker tracker;
    unsigned content = 0, diagnostics = 0, rounding_events = 0;
    bool rendering = false;
};
alignas(64) static uint32_t draw[320 * 12];
static void Diagnostic(lv_obj_t* screen, State& state, const lv_area_t& area) {
    state.tracker.BeginDiagnosticInvalidation();
    lv_obj_invalidate_area(screen, &area);
    state.tracker.EndDiagnosticInvalidation();
}
int main() {
    lv_init();
    auto* display = lv_display_create(320, 240);
    State state;
    lv_display_set_user_data(display, &state);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_XRGB8888);
    lv_display_set_buffers(display, draw, nullptr, sizeof(draw), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, [](lv_display_t* d, const lv_area_t*, uint8_t*) {
        auto& s = *static_cast<State*>(lv_display_get_user_data(d));
        if (lv_display_flush_is_last(d)) {
            if (s.tracker.content()) ++s.content;
            else ++s.diagnostics;
        }
        lv_display_flush_ready(d);
    });
    lv_display_add_event_cb(display, [](lv_event_t* event) {
        auto& s = *static_cast<State*>(lv_event_get_user_data(event));
        switch (lv_event_get_code(event)) {
            case LV_EVENT_INVALIDATE_AREA:
                if (s.rendering) ++s.rounding_events;
                s.tracker.Invalidated(); break;
            case LV_EVENT_RENDER_START:
                s.rendering = true; s.tracker.BeginRender(); break;
            case LV_EVENT_RENDER_READY:
                s.tracker.EndRender(); s.rendering = false; break;
            default: break;
        }
    }, LV_EVENT_ALL, &state);
    auto* screen = lv_display_get_screen_active(display);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x183040), 0);
    lv_refr_now(display);
    assert(state.content == 1 && state.diagnostics == 0);
    const lv_area_t hud = {130, 0, 189, 15};
    Diagnostic(screen, state, hud);
    lv_refr_now(display);
    assert(state.content == 1 && state.diagnostics == 1 && state.rounding_events > 0);
    // Content must still count when its invalidation is completely contained
    // in an already pending diagnostic region (LVGL merges these requests).
    Diagnostic(screen, state, hud);
    lv_obj_invalidate_area(screen, &hud);
    lv_refr_now(display);
    assert(state.content == 2 && state.diagnostics == 1);
    lv_obj_invalidate_area(screen, &hud);
    Diagnostic(screen, state, hud);
    lv_refr_now(display);
    assert(state.content == 3 && state.diagnostics == 1);
    Diagnostic(screen, state, hud);
    Diagnostic(screen, state, hud);
    lv_refr_now(display);
    assert(state.content == 3 && state.diagnostics == 2);
    lv_refr_now(display); // Idle: no manufactured content frame.
    assert(state.content == 3 && state.diagnostics == 2);
    lv_obj_invalidate(screen); // A real Surface/UI update still counts.
    lv_refr_now(display);
    assert(state.content == 4 && state.diagnostics == 2);
    lv_deinit();
    std::puts("DOIT GameKit real LVGL diagnostic/content refresh tests passed");
}
