#include <cassert>
#include <cstdio>
#include <vector>

#include "esp_mosaico_refresh.h"

namespace {
unsigned flushes = 0, renders = 0;
void Flush(lv_display_t* display, const lv_area_t*, uint8_t*) {
    ++flushes;
    lv_display_flush_ready(display);
}
void Animate(void* object, int32_t value) { lv_obj_set_x(static_cast<lv_obj_t*>(object), value); }
}

int main() {
    lv_init();
    auto* display = lv_display_create(480, 480);
    std::vector<uint8_t> pixels(480 * 480 * 2);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, pixels.data(), nullptr, pixels.size(), LV_DISPLAY_RENDER_MODE_DIRECT);
    assert(lv_display_get_buf_active(display) != nullptr);
    lv_display_set_flush_cb(display, Flush);
    lv_display_add_event_cb(display, [](lv_event_t*) { ++renders; }, LV_EVENT_RENDER_START, nullptr);
    auto* screen = lv_display_get_screen_active(display);
    auto* label = lv_label_create(screen);
    auto* overlay = lv_obj_create(lv_display_get_layer_top(display));
    lv_obj_set_size(overlay, 120, 80);
    lv_obj_set_hidden(overlay, true);
    lv_anim_delete_all();
    for (unsigned frame = 0; frame < 100; ++frame) {
        lv_label_set_text_fmt(label, "native overlay %u", frame);
        lv_obj_set_pos(label, frame, frame);
        assert(mosaico_board::PresentAndAcknowledgeRefresh(display, [&] {
            // The compositor sees final geometry, not last frame's layout.
            lv_area_t area;
            lv_obj_get_coords(label, &area);
            assert(area.x1 == int(frame) && area.y1 == int(frame));
            assert(lv_obj_get_width(label) > 0);
            return true;
        }));
        lv_refr_now(display);
        assert(flushes == 0 && renders == 0);
    }
    // Failed/unsafe direct presentation leaves trusted UI's refresh intact.
    lv_obj_set_hidden(overlay, false);
    assert(!mosaico_board::PresentAndAcknowledgeRefresh(display, [] { return false; }));
    lv_refr_now(display);
    assert(flushes > 0 && renders > 0);
    const auto ordinary_flushes = flushes;
    lv_obj_set_hidden(overlay, true);
    assert(mosaico_board::PresentAndAcknowledgeRefresh(display, [] { return true; }));
    lv_refr_now(display);
    assert(flushes == ordinary_flushes);

    // Acknowledging the current frame never cancels future animation updates.
    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, label);
    lv_anim_set_exec_cb(&animation, Animate);
    lv_anim_set_values(&animation, 0, 255);
    lv_anim_set_duration(&animation, 1000);
    lv_anim_start(&animation);
    lv_obj_invalidate(label);
    assert(lv_anim_count_running() != 0);
    assert(mosaico_board::PresentAndAcknowledgeRefresh(display, [] { return true; }));
    lv_tick_inc(20);
    lv_refr_now(display);
    assert(flushes > ordinary_flushes);
    lv_anim_delete_all();
    bool called = false;
    assert(!mosaico_board::PresentAndAcknowledgeRefresh(nullptr, [&] { called = true; return true; }));
    assert(!called);
    // The refresh timer can run before the asynchronous presenter. Intercept
    // that ordering before RENDER_START; already-acknowledged queues do no work.
    unsigned early_frames = 0;
    bool eligible = true;
    struct Gate { unsigned* count; bool* eligible; } gate{&early_frames, &eligible};
    lv_display_add_event_cb(display, [](lv_event_t* event) {
        auto* state = static_cast<Gate*>(lv_event_get_user_data(event));
        mosaico_board::PresentPendingRefresh(static_cast<lv_display_t*>(lv_event_get_target(event)), [state] {
            if (!*state->eligible) return false;
            ++*state->count;
            return true;
        });
    }, LV_EVENT_REFR_START, &gate);
    const auto before_early = flushes;
    for (unsigned frame = 0; frame < 100; ++frame) {
        lv_label_set_text_fmt(label, "early overlay %u", frame);
        lv_refr_now(display);
    }
    assert(early_frames == 100 && flushes == before_early);
    lv_refr_now(display);
    assert(early_frames == 100);
    eligible = false;
    lv_obj_set_hidden(overlay, false);
    lv_refr_now(display);
    assert(flushes > before_early);
    lv_display_delete(display);
    lv_deinit();
    puts("Mosaico direct refresh: 200 overlays skip duplicate rendering; both worker/timer orders, geometry, trusted fallback and future animation ticks passed");
}
