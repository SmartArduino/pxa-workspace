#include <cassert>
#include <cstdio>
#include <vector>

#include <lvgl.h>
#include "esp_mosaico_transfer.h"
#include "esp_mosaico_input.h"
#include "esp_mosaico_input_lvgl.h"

namespace {
class Display {
public:
    explicit Display(unsigned rows, bool retained = false, bool native = false, bool deferred = false)
        : draw_{std::vector<uint8_t>(480 * rows * 2), std::vector<uint8_t>(480 * rows * 2)},
          frame_(480 * 480 * 2), snapshot_(frame_.size()), retained_(retained), native_(native), deferred_(deferred) {
        display_ = lv_display_create(480, 480);
        assert(display_);
        lv_display_set_default(display_);
        lv_display_set_color_format(display_, native ? LV_COLOR_FORMAT_RGB565 : LV_COLOR_FORMAT_RGB565_SWAPPED);
        lv_display_set_buffers(display_, draw_[0].data(), retained ? nullptr : draw_[1].data(), draw_[0].size(),
                               retained ? LV_DISPLAY_RENDER_MODE_DIRECT : LV_DISPLAY_RENDER_MODE_PARTIAL);
        lv_display_set_driver_data(display_, this);
        lv_display_set_flush_cb(display_, Flush);
        if (deferred_) lv_display_set_flush_wait_cb(display_, [](lv_display_t* display) {
            static_cast<Display*>(lv_display_get_driver_data(display))->CompleteSnapshot();
        });
        lv_display_add_event_cb(display_, [](lv_event_t* event) {
            mosaico_board::RoundDisplayArea(static_cast<lv_area_t*>(lv_event_get_param(event)));
        }, LV_EVENT_INVALIDATE_AREA, nullptr);
        lv_display_add_event_cb(display_, [](lv_event_t* event) {
            auto* self = static_cast<Display*>(lv_event_get_user_data(event));
            // The production RENDER_START guard also covers callers that
            // acquire the display lock before the previous worker completes.
            self->CompleteSnapshot();
            if (self->retained_ && !self->ui_current_) {
                const lv_area_t full = {0, 0, 479, 479};
                assert(mosaico_board::CopyRefreshArea(full, self->snapshot_.data(), self->draw_[0].data(), self->native_));
                self->ui_current_ = true;
            }
        }, LV_EVENT_RENDER_START, this);
        auto* screen = lv_display_get_screen_active(display_);
        lv_obj_set_style_bg_color(screen, lv_color_hex(0x172338), 0);
        container_ = lv_obj_create(screen);
        lv_obj_set_pos(container_, 16, 40);
        lv_obj_set_size(container_, 448, 420);
        lv_obj_set_style_pad_all(container_, 8, 0);
        lv_obj_set_style_radius(container_, 20, 0);
        lv_obj_set_scrollbar_mode(container_, LV_SCROLLBAR_MODE_OFF);
        for (int i = 0; i < 12; ++i) {
            auto* card = lv_obj_create(container_);
            lv_obj_set_pos(card, 0, i * 72);
            lv_obj_set_size(card, 430, 64);
            lv_obj_set_style_radius(card, 15, 0);
            lv_obj_set_style_bg_color(card, lv_color_hex(0xacc8dc + i * 0x30100), 0);
            auto* label = lv_label_create(card);
            lv_label_set_text_fmt(label, "Settings row %d", i);
        }
        indicator_ = lv_obj_create(screen);
        lv_obj_set_pos(indicator_, 200, 2);
        lv_obj_set_size(indicator_, 80, 32);
        overlay_ = lv_obj_create(screen);
        lv_obj_set_pos(overlay_, 160, 170);
        lv_obj_set_size(overlay_, 200, 160);
        lv_obj_set_style_bg_color(overlay_, lv_color_hex(0x6090e0), 0);
        lv_obj_set_style_bg_opa(overlay_, LV_OPA_70, 0);
        lv_obj_set_style_radius(overlay_, 22, 0);
        lv_obj_set_hidden(overlay_, true);
    }
    ~Display() { CompleteSnapshot(); lv_display_delete(display_); }
    Display(const Display&) = delete;
    Display& operator=(const Display&) = delete;

    void Render(int stage) {
        lv_obj_scroll_to_y(container_, stage * 13, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(indicator_, lv_color_hex(0x306090 + stage * 0x50103), 0);
        lv_obj_set_hidden(overlay_, stage % 3 != 1);
        lv_refr_now(display_);
    }
    const std::vector<uint8_t>& pixels() const { return snapshot_; }
    bool pending() const { return pending_pixels_ != nullptr; }
    void SignalReadyBeforeOwnershipRelease() {
        assert(pending());
        lv_display_flush_ready(display_);
    }
    void CompleteSnapshot() {
        if (!pending_pixels_) return;
        // Defer the worker deliberately. The single retained source must remain
        // byte-exact while LVGL updates/layouts the next frame or changes caller.
        assert(std::memcmp(pending_pixels_, pending_bytes_.data(), pending_bytes_.size()) == 0);
        assert(mosaico_board::CopyRefreshArea(pending_area_, pending_pixels_, snapshot_.data(), native_));
        pending_pixels_ = nullptr;
        lv_display_flush_ready(display_);
    }
    unsigned flushes() const { return flushes_; }
    unsigned refreshes() const { return refreshes_; }
    void SimulateDirectGame() {
        CompleteSnapshot(); // Direct presenter must not share SRM with a pending UI copy.
        for (size_t i = 0; i < snapshot_.size(); ++i) snapshot_[i] = (i * 37) ^ (i >> 9);
        frame_ = snapshot_;
        ui_current_ = false;
    }

private:
    static void Flush(lv_display_t* display, const lv_area_t* area, uint8_t* pixels) {
        auto* self = static_cast<Display*>(lv_display_get_driver_data(display));
        const auto* active = lv_display_get_buf_active(display);
        if (self->retained_) assert(self->dirty_.Include(*area));
        else assert(self->dirty_.Merge(*area, pixels, active->header.stride, self->frame_.data()));
        ++self->flushes_;
        if (lv_display_flush_is_last(display)) {
            lv_area_t dirty;
            assert(self->dirty_.Take(&dirty));
            ++self->refreshes_;
            if (self->deferred_) {
                assert(self->retained_ && !self->pending());
                self->pending_area_ = dirty;
                self->pending_pixels_ = pixels;
                self->pending_bytes_.assign(pixels, pixels + self->draw_[0].size());
                return;
            }
            assert(mosaico_board::CopyRefreshArea(dirty,
                self->retained_ ? pixels : self->frame_.data(), self->snapshot_.data(), self->native_));
        }
        lv_display_flush_ready(display);
    }
    std::vector<uint8_t> draw_[2], frame_, snapshot_;
    mosaico_board::RefreshRegion dirty_;
    lv_display_t* display_ = nullptr;
    lv_obj_t* container_ = nullptr;
    lv_obj_t* indicator_ = nullptr;
    lv_obj_t* overlay_ = nullptr;
    unsigned flushes_ = 0, refreshes_ = 0;
    bool retained_ = false;
    bool native_ = false;
    bool ui_current_ = true;
    bool deferred_ = false;
    lv_area_t pending_area_ = {};
    const uint8_t* pending_pixels_ = nullptr;
    std::vector<uint8_t> pending_bytes_;
};

void TestLvglInputTakeover() {
    Display output(mosaico_board::kDrawBufferRows);
    auto* display = lv_display_get_default();
    auto* screen = lv_display_get_screen_active(display);
    unsigned clicks[2] = {};
    for (int slot = 0; slot < 2; ++slot) {
        auto* button = lv_button_create(screen);
        lv_obj_set_pos(button, 40 + slot * 240, 120);
        lv_obj_set_size(button, 120, 120);
        lv_obj_add_event_cb(button, [](lv_event_t* event) {
            ++*static_cast<unsigned*>(lv_event_get_user_data(event));
        }, LV_EVENT_CLICKED, &clicks[slot]);
    }
    lv_obj_update_layout(screen);
    EspMosaicoInput router;
    struct Context { EspMosaicoInput* router; size_t slot; } contexts[] = {{&router, 0}, {&router, 1}};
    lv_indev_t* pointers[2];
    for (size_t slot = 0; slot < 2; ++slot) {
        pointers[slot] = lv_indev_create();
        lv_indev_set_type(pointers[slot], LV_INDEV_TYPE_POINTER);
        lv_indev_set_disp(pointers[slot], display);
        lv_indev_set_user_data(pointers[slot], &contexts[slot]);
        lv_indev_set_read_cb(pointers[slot], [](lv_indev_t* input, lv_indev_data_t* data) {
            auto* context = static_cast<Context*>(lv_indev_get_user_data(input));
#if LV_USE_GESTURE_RECOGNITION
            if (context->slot == 0) {
                EspMosaicoTouchPoint points[EspMosaicoInput::kPointers] = {};
                const auto count = context->router->Points(points);
                lv_indev_touch_data_t touches[EspMosaicoInput::kPointers] = {};
                for (size_t index = 0; index < count; ++index) {
                    touches[index].state = LV_INDEV_STATE_PRESSED;
                    touches[index].point = {points[index].x, points[index].y};
                    touches[index].id = points[index].id;
                    touches[index].timestamp = lv_tick_get();
                }
                lv_indev_gesture_recognizers_update(input, touches, count);
            }
#endif
            const auto sample = context->router->Read(context->slot);
            if (sample.cancelled) lv_indev_wait_release(input);
            data->state = sample.event.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
            data->point = {sample.event.point.x, sample.event.point.y};
            data->continue_reading = context->router->pending(context->slot);
            if (context->slot == 0) MosaicoSetGestureMetadata(input, data);
        });
    }
    auto pump = [&]() {
        lv_tick_inc(10);
        for (auto* pointer : pointers) lv_indev_read(pointer);
    };
    // Complete taps between reads retain both endpoints, for both pointer IDs.
    for (uint8_t id = 0; id < 2; ++id) {
        assert(router.Inject(id, 80 + id * 240, 160, true, 100));
        assert(router.Inject(id, 80 + id * 240, 160, false, 110));
    }
    pump();
    assert(clicks[0] == 1 && clicks[1] == 1);
    assert(router.Inject(0, 80, 160, true, 120));
    pump();
    assert(router.Cancel(0, 130));
    pump();
    assert(clicks[0] == 1); // Cancellation is not a click.
    assert(router.Inject(0, 80, 160, true, 140));
    pump();
    // Flood a busy UI while its old press is on button 0. The retained
    // cancellation must not click that button; all new taps are outside it.
    for (int i = 0; i < 9; ++i) {
        assert(router.Inject(0, 200, 400, false, 141 + i * 2));
        assert(router.Inject(0, 200, 400, true, 142 + i * 2));
    }
    pump();
    assert(clicks[0] == 1);
    assert(router.Cancel(0, 170));
    pump();
    assert(router.Inject(0, 80, 160, true, 180));
    pump();
    const EspMosaicoTouchPoint physical[] = {{320, 160, 7}};
    assert(router.UpdatePhysical(physical, 1, 190, true));
    pump();
    assert(clicks[0] == 1);
    // A command already in flight when physical input takes over must not
    // steal ownership back, or release the real finger.
    assert(!router.Inject(0, 80, 160, true, 195));
    assert(router.Inject(0, 80, 160, false, 196));
    pump();
    assert(clicks[0] == 1 && clicks[1] == 1);
    assert(!router.UpdatePhysical(nullptr, 0, 200, true));
    pump();
    assert(clicks[1] == 2); // Physical input resumes, without clicking the remote target.
    for (auto* pointer : pointers) lv_indev_delete(pointer);
    std::printf("Real LVGL: dual remote taps, cancellation and physical takeover passed; recognizers=%d.\n",
        LV_USE_GESTURE_RECOGNITION);
}
}

int main() {
    lv_init();
    {
        Display full(mosaico_board::kHeight), half(mosaico_board::kHeight / 2),
            retained(mosaico_board::kDrawBufferRows, true),
            native(mosaico_board::kDrawBufferRows, true, true);
        for (int stage = 0; stage < 30; ++stage) {
            full.Render(stage);
            half.Render(stage);
            retained.Render(stage);
            native.Render(stage);
            assert(full.pixels() == half.pixels());
            assert(full.pixels() == retained.pixels());
            assert(full.pixels() == native.pixels());
        }
        assert(full.refreshes() == half.refreshes() && half.refreshes() == 30);
        assert(half.flushes() > full.flushes());
        assert(retained.refreshes() == 30);
        std::printf("Real LVGL: 30 scrolling/alpha/rounded UI frames match byte-for-byte; "
                    "full-height flushes=%u, half-height flushes=%u, retained flushes=%u.\n",
                    full.flushes(), half.flushes(), retained.flushes());
        for (int stage = 30; stage < 34; ++stage) {
            full.SimulateDirectGame(); half.SimulateDirectGame(); retained.SimulateDirectGame(); native.SimulateDirectGame();
            full.Render(stage); half.Render(stage); retained.Render(stage); native.Render(stage);
            assert(full.pixels() == half.pixels());
            assert(full.pixels() == retained.pixels());
            assert(full.pixels() == native.pixels());
        }
        std::puts("Real LVGL: direct game to partial UI restores before drawing and preserves untouched pixels.");
    }
    {
        Display reference(480, true, true), deferred(480, true, true, true);
        for (int stage = 0; stage < 30; ++stage) {
            reference.Render(stage); deferred.Render(stage);
            assert(deferred.pending());
            deferred.CompleteSnapshot();
            assert(reference.pixels() == deferred.pixels());
        }
        // Exercise a next refresh before the old worker completes. Checking
        // the entire source catches even writes outside the old dirty region.
        deferred.Render(30); assert(deferred.pending());
        deferred.SignalReadyBeforeOwnershipRelease();
        deferred.Render(31); deferred.CompleteSnapshot();
        reference.Render(31);
        assert(reference.pixels() == deferred.pixels());
        deferred.Render(32); assert(deferred.pending());
        deferred.SimulateDirectGame(); reference.SimulateDirectGame();
        reference.Render(33); deferred.Render(33); deferred.CompleteSnapshot();
        assert(reference.pixels() == deferred.pixels());
        std::puts("Real LVGL: deferred snapshots preserve source ownership, next refresh and game-to-UI restoration.");
    }
    TestLvglInputTakeover();
    lv_deinit();
}
