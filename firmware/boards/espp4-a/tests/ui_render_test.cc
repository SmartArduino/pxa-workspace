// SPDX-License-Identifier: Apache-2.0
#include "espp4_a_ui_render.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
constexpr int kWidth = 480, kHeight = 854;
constexpr size_t kPixels = size_t(kWidth) * kHeight, kBytes = kPixels * 2;
struct Display {
    lv_display_t* display;
    espp4_a::UiRenderMode mode;
    uint16_t* buffers[2];
    uint16_t* front;
    lv_obj_t *panel, *marker[2], *clock;
    uint64_t damaged = 0;
    unsigned submitted = 0, partial = 0;
    bool guest = false;
    bool overlay = false;

    static void Flush(lv_display_t* display, const lv_area_t* area, uint8_t* pixels) {
        auto& self = *static_cast<Display*>(lv_display_get_user_data(display));
        self.damaged += uint64_t(lv_area_get_width(area)) * lv_area_get_height(area);
        if (self.mode.ShouldSubmit(display)) {
            self.front = reinterpret_cast<uint16_t*>(pixels);
            assert(self.front == self.buffers[0] || self.front == self.buffers[1]);
            if (self.guest && !self.mode.direct())
                for (int y = 700; y < 710; ++y)
                    std::fill_n(self.front + y * kWidth, 16, uint16_t(0xf81f));
            if (self.overlay)
                for (int y = 0; y < 16; ++y)
                    std::fill_n(self.front + y * kWidth + 210, 60, uint16_t(0xffff));
            self.mode.OverlayDrawn(self.overlay);
            ++self.submitted;
        } else ++self.partial;
        lv_display_flush_ready(display);
    }
    static void Plain(lv_obj_t* object, uint32_t color) {
        lv_obj_remove_style_all(object);
        lv_obj_set_style_bg_color(object, lv_color_hex(color), 0);
        lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    }
    Display() {
        for (auto& buffer : buffers) {
            buffer = static_cast<uint16_t*>(aligned_alloc(LV_DRAW_BUF_ALIGN, kBytes));
            assert(buffer); std::memset(buffer, 0, kBytes);
        }
        front = buffers[0];
        display = lv_display_create(kWidth, kHeight);
        lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
        lv_display_set_user_data(display, this);
        lv_display_set_flush_cb(display, Flush);
        lv_display_add_event_cb(display, [](lv_event_t* event) {
            auto& self = *static_cast<Display*>(lv_event_get_user_data(event));
            self.mode.IncludeOverlay(static_cast<lv_area_t*>(lv_event_get_param(event)),
                self.overlay, lv_area_t{210, 0, 269, 15});
        }, LV_EVENT_INVALIDATE_AREA, this);
        mode.Configure(display, buffers[1], buffers[0], kBytes, false);
        auto* screen = lv_display_get_screen_active(display);
        Plain(screen, 0x10121a);
        panel = lv_obj_create(screen);
        Plain(panel, 0x252b3a);
        lv_obj_set_size(panel, 400, 480);
        lv_obj_set_pos(panel, 40, -400);
        lv_obj_set_style_radius(panel, 18, 0);
        for (int row = 0; row < 8; ++row) {
            auto* tile = lv_obj_create(panel);
            Plain(tile, row % 2 ? 0x354156 : 0x40536b);
            lv_obj_set_pos(tile, 20, row * 52 + 10);
            lv_obj_set_size(tile, 360, 42);
            lv_obj_set_style_radius(tile, 8, 0);
            auto* label = lv_label_create(tile);
            lv_label_set_text(label, "Wi-Fi  Notifications  Brightness");
            lv_obj_set_style_text_color(label, lv_color_white(), 0);
            lv_obj_center(label);
        }
        for (int i = 0; i < 2; ++i) {
            marker[i] = lv_obj_create(screen);
            Plain(marker[i], 0x1565c0);
            lv_obj_set_pos(marker[i], 10 + i * 430, 760);
            lv_obj_set_size(marker[i], 20, 20);
        }
        auto* bar = lv_obj_create(lv_display_get_layer_top(display));
        Plain(bar, 0x141821);
        lv_obj_set_size(bar, kWidth, 28);
        clock = lv_label_create(bar);
        lv_label_set_text(clock, "12:00");
        lv_obj_set_style_text_color(clock, lv_color_white(), 0);
        lv_obj_set_pos(clock, 16, 4);
    }
    ~Display() {
        lv_display_delete(display);
        for (auto* buffer : buffers) std::free(buffer);
    }
    void Select(bool enabled, bool overlay = false) {
        const bool overlay_changed = this->overlay != overlay;
        this->overlay = overlay;
        const bool direct = mode.Desired(enabled, guest, 0);
        if (mode.NeedsChange(direct))
            mode.Configure(display, front == buffers[0] ? buffers[1] : buffers[0],
                           front, kBytes, direct);
        if (overlay_changed) lv_obj_invalidate(marker[0]);
    }
    void Draw() {
        const auto before = submitted;
        lv_refr_now(display);
        // Several dirty rectangles must never cause several DMA submissions.
        assert(submitted - before <= 1);
    }
    void Move(unsigned step) {
        lv_obj_set_y(panel, -400 + int(step % 41) * 12);
        for (int i = 0; i < 2; ++i)
            lv_obj_set_x(marker[i], 10 + i * 430 + int(step % 3));
        if (step % 5 == 0) lv_label_set_text_fmt(clock, "12:%02u", step % 60);
    }
};
void Same(const Display& baseline, const Display& candidate) {
    assert(std::memcmp(baseline.front, candidate.front, kBytes) == 0);
}
} // namespace

int main() {
    lv_init();
    {
        Display baseline, candidate;
        candidate.Select(true);
        baseline.Draw(); candidate.Draw(); Same(baseline, candidate);
        baseline.damaged = candidate.damaged = 0;
        for (unsigned step = 1; step < 82; ++step) {
            baseline.Move(step); candidate.Move(step);
            baseline.Draw(); candidate.Draw(); Same(baseline, candidate);
        }
        assert(candidate.partial > 0);
        assert(candidate.damaged < baseline.damaged * 9 / 10);
        std::printf("Native UI: FULL=%llu DIRECT=%llu damaged pixels; identical completed images; %u non-final flushes withheld\n",
            (unsigned long long)baseline.damaged, (unsigned long long)candidate.damaged, candidate.partial);
        for (unsigned cycle = 0; cycle < 5; ++cycle) {
            // A FULL compositor is allowed to modify its target after LVGL.
            baseline.guest = candidate.guest = true;
            candidate.Select(true);
            assert(!candidate.mode.direct());
            lv_obj_invalidate(lv_display_get_screen_active(baseline.display));
            baseline.Draw(); candidate.Draw(); Same(baseline, candidate);
            assert(candidate.front[700 * kWidth] == 0xf81f);
            // Returning to retained native pixels must remove that guest from
            // both alternating buffers, including subsequent small updates.
            baseline.guest = candidate.guest = false;
            candidate.Select(true);
            lv_obj_invalidate(lv_display_get_screen_active(baseline.display));
            baseline.Draw(); candidate.Draw(); Same(baseline, candidate);
            for (unsigned frame = 1; frame < 4; ++frame) {
                baseline.Move(frame + cycle); candidate.Move(frame + cycle);
                baseline.Draw(); candidate.Draw(); Same(baseline, candidate);
            }
            for (auto* buffer : candidate.buffers) assert(buffer[700 * kWidth] != 0xf81f);
            // A late guest request forces one safe FULL pass. Diagnostic
            // overlays must be repainted before retained pixel reuse.
            candidate.mode.RequireFull(); candidate.Select(true);
            assert(!candidate.mode.direct());
            baseline.Draw(); candidate.Draw(); Same(baseline, candidate);
            candidate.Select(true); candidate.Draw(); Same(baseline, candidate);
        }
        for (bool visible : {true, false, true, false}) {
            baseline.Select(false, visible); candidate.Select(true, visible);
            assert(candidate.mode.direct());
            for (unsigned frame = 0; frame < 8; ++frame) {
                baseline.Move(frame); candidate.Move(frame);
                baseline.Draw(); candidate.Draw(); Same(baseline, candidate);
            }
        }
        assert(!candidate.mode.Desired(true, false, 1));
    }
    lv_deinit();
    std::puts("P4 native UI: dirty rectangles, multi-area handoff, guest/shell transitions and overlay on/off passed");
}
