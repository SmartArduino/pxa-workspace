#pragma once
#include "doit_gamekit_config.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <lvgl.h>
#include "src/misc/lv_event_private.h"
#include "src/widgets/buttonmatrix/lv_buttonmatrix_private.h"

namespace doit_gamekit {
// A real LVGL keypad/group adapter. Normal controls use spatial focus;
// widgets with an internal selection receive their native LVGL key events.
// All methods run under the display/LVGL locks, on the UI task.
class KeyNavigation {
public:
    bool Initialize(lv_display_t* display) {
        display_ = display;
        group_ = lv_group_create();
        keypad_ = lv_indev_create();
        if (!group_ || !keypad_) return false;
        lv_style_init(&outline_);
        lv_style_set_outline_color(&outline_, lv_color_hex(0xffb74d));
        lv_style_set_outline_width(&outline_, 2);
        // Keep the whole ring inside the widget when its parent clips at the edge.
        lv_style_set_outline_pad(&outline_, -2);
        lv_style_set_outline_opa(&outline_, LV_OPA_COVER);
        lv_style_init(&matrix_outline_);
        lv_style_set_outline_width(&matrix_outline_, 0);
        lv_indev_set_type(keypad_, LV_INDEV_TYPE_KEYPAD);
        lv_indev_set_disp(keypad_, display);
        lv_indev_set_group(keypad_, group_);
        lv_group_set_user_data(group_, this);
        lv_group_set_focus_cb(group_, [](lv_group_t* group) {
            static_cast<KeyNavigation*>(lv_group_get_user_data(group))->NormalizeFocus();
        });
        lv_indev_set_user_data(keypad_, this);
        lv_indev_set_read_cb(keypad_, [](lv_indev_t* device, lv_indev_data_t* data) {
            auto* self = static_cast<KeyNavigation*>(lv_indev_get_user_data(device));
            data->key = self->key_;
            data->state = self->pressed_ ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
            data->timestamp = lv_tick_get();
        });
        // GPIO polling already runs on the LVGL task. Read synchronously so a
        // mode/page change can cancel Enter before another object sees it.
        lv_indev_set_mode(keypad_, LV_INDEV_MODE_EVENT);
        lv_indev_set_long_press_time(keypad_, 400);
        lv_indev_set_long_press_repeat_time(keypad_, 120);
        return true;
    }

    void Cancel() {
        if (keypad_) {
            auto* previous = pressed_object_;
            const bool was_pressed = pressed_;
            pressed_ = false;
            SetPressedObject(nullptr);
            if (was_pressed && previous)
                lv_obj_send_event(previous, LV_EVENT_PRESS_LOST, keypad_);
            lv_indev_reset(keypad_, nullptr);
            lv_indev_wait_release(keypad_);
            pressed_ = false;
            Emit(key_, false);
        }
        held_ = 0;
    }

    void SetTextActivation(void* context, void (*activate)(void*, lv_obj_t*)) {
        activate_context_ = context;
        activate_text_ = activate;
    }

    void Deinitialize() {
        if (!group_) return;
        Cancel();
        Refresh(nullptr, false, lv_tick_get());
        lv_indev_delete(keypad_);
        lv_group_delete(group_);
        lv_style_reset(&outline_);
        lv_style_reset(&matrix_outline_);
        keypad_ = nullptr;
        group_ = nullptr;
    }

    void Update(lv_obj_t* scope, bool enabled, uint32_t buttons, uint32_t now) {
        if (!group_) return;
        if (enabled != enabled_ || scope != scope_ || now - refreshed_ >= 50 || buttons != held_)
            Refresh(scope, enabled, now);
        if (!enabled_) { held_ = 0; return; }
        if (pressed_ && pressed_object_ != focused()) {
            Cancel();
            blocked_ = buttons != 0;
        }
        if (blocked_) {
            if (!buttons) blocked_ = false;
            held_ = buttons;
            Emit(key_, false);
            return;
        }
        lv_obj_t* focused = lv_group_get_focused(group_);
        if (!focused) { held_ = buttons; return; }
        NormalizeFocus();
        bool delete_action = false;
        if ((buttons & X) && !(held_ & X))
            for (uint32_t i = 0; i < lv_obj_get_event_count(focused); ++i)
                delete_action |= (lv_obj_get_event_dsc(focused, i)->filter &
                                  ~LV_EVENT_PREPROCESS) == LV_EVENT_KEY;
        if (delete_action) {
            if (pressed_) Cancel();
            // Native task cards use Delete to close; game-owned controller
            // input bypasses this keypad adapter entirely.
            uint32_t key = LV_KEY_DEL;
            lv_obj_send_event(focused, LV_EVENT_KEY, &key);
            held_ = buttons;
            return;
        }
        if (buttons & A) {
            const bool activate_editor = !(held_ & A) && activate_text_ &&
                lv_obj_has_class(focused, &lv_textarea_class);
            if (pressed_ && key_ != LV_KEY_ENTER) Emit(key_, false);
            Emit(LV_KEY_ENTER, true);
            if (activate_editor && pressed_object_ == focused) activate_text_(activate_context_, focused);
        } else {
            const uint32_t directions = buttons & (Up | Down | Left | Right | L | R);
            const uint32_t direction = directions & (0u - directions);
            const uint32_t held_directions = held_ & (Up | Down | Left | Right | L | R);
            const uint32_t previous_direction = held_directions & (0u - held_directions);
            uint32_t key = direction == Up ? LV_KEY_UP : direction == Down ? LV_KEY_DOWN :
                direction == Left ? LV_KEY_LEFT : direction == Right ? LV_KEY_RIGHT : 0;
            if (key && InternalDirection(focused, key)) {
                if (pressed_ && key_ != key) Emit(key_, false);
                Emit(key, true);
            } else {
                if (pressed_) {
                    const bool commit_slider = key_ != LV_KEY_ENTER &&
                        lv_obj_has_class(focused, &lv_slider_class);
                    Emit(key_, false);
                    if (commit_slider) lv_obj_send_event(focused, LV_EVENT_RELEASED, nullptr);
                }
                if (direction && (direction != previous_direction ||
                                   uint32_t(now - repeated_) >= repeat_delay_)) {
                    const bool first = direction != previous_direction;
                    repeated_ = now;
                    repeat_delay_ = first ? 400 : 120;
                    if (direction == L || direction == R) Step(direction == R);
                    else Move(key);
                }
            }
        }
        held_ = buttons;
    }

    bool ClosePopup() {
        auto* focused = group_ ? lv_group_get_focused(group_) : nullptr;
        if (!focused || !lv_obj_has_class(focused, &lv_dropdown_class) ||
            !lv_dropdown_is_open(focused)) return false;
        Cancel();
        lv_dropdown_close(focused);
        return true;
    }

    lv_obj_t* focused() const { return group_ ? lv_group_get_focused(group_) : nullptr; }
    lv_indev_t* keypad() const { return keypad_; }

private:
    void NormalizeFocus() {
        auto* object = focused();
        if (!object) return;
        // LVGL's programmatic group focus can pick a pointer indev even when
        // it belongs to no group. Keep the keypad appearance and initialize
        // keyboard selection explicitly when both input types coexist.
        if (!lv_obj_has_state(object, LV_STATE_FOCUS_KEY))
            lv_obj_add_state(object, LV_STATE_FOCUS_KEY);
        if (IsMatrix(object) && !ButtonEnabled(object, lv_buttonmatrix_get_selected_button(object))) {
            for (uint32_t i = 0; lv_buttonmatrix_get_button_text(object, i); ++i) {
                if (lv_buttonmatrix_has_button_ctrl(object, i, LV_BUTTONMATRIX_CTRL_HIDDEN) ||
                    lv_buttonmatrix_has_button_ctrl(object, i, LV_BUTTONMATRIX_CTRL_DISABLED)) continue;
                lv_buttonmatrix_set_selected_button(object, i);
                break;
            }
        }
    }

    static bool InternalDirection(lv_obj_t* object, uint32_t key) {
        if (lv_obj_has_class(object, &lv_slider_class) || lv_obj_has_class(object, &lv_spinbox_class) ||
            lv_obj_has_class(object, &lv_textarea_class))
            return key == LV_KEY_LEFT || key == LV_KEY_RIGHT;
        if (lv_obj_has_class(object, &lv_dropdown_class)) return lv_dropdown_is_open(object);
        // Matrix cells participate in spatial navigation, including movement
        // across separate candidate, typing and toolbar matrices.
        return false;
    }

    static bool IsMatrix(lv_obj_t* object) {
        return lv_obj_has_class(object, &lv_buttonmatrix_class);
    }

    static bool ButtonEnabled(lv_obj_t* object, uint32_t button) {
        return lv_buttonmatrix_get_button_text(object, button) &&
            !lv_buttonmatrix_has_button_ctrl(object, button, LV_BUTTONMATRIX_CTRL_HIDDEN) &&
            !lv_buttonmatrix_has_button_ctrl(object, button, LV_BUTTONMATRIX_CTRL_DISABLED);
    }

    static lv_area_t ButtonArea(lv_obj_t* object, uint32_t button) {
        lv_area_t bounds;
        lv_obj_get_coords(object, &bounds);
        // Pinned LVGL 9.6 has no public virtual-button geometry getter.
        const auto cell = reinterpret_cast<lv_buttonmatrix_t*>(object)->button_areas[button];
        return {bounds.x1 + cell.x1, bounds.y1 + cell.y1,
                bounds.x1 + cell.x2, bounds.y1 + cell.y2};
    }

    void FocusStyle(lv_obj_t* object, bool add) {
        const auto state = LV_STATE_FOCUSED | LV_STATE_FOCUS_KEY;
        if (IsMatrix(object)) {
            // Highlight the selected virtual key, never the enclosing panel.
            if (add) {
                lv_obj_add_style(object, &matrix_outline_, state);
                lv_obj_add_style(object, &outline_, LV_PART_ITEMS | state);
            } else {
                lv_obj_remove_style(object, &matrix_outline_, state);
                lv_obj_remove_style(object, &outline_, LV_PART_ITEMS | state);
            }
        } else if (add) lv_obj_add_style(object, &outline_, state);
        else lv_obj_remove_style(object, &outline_, state);
    }

    void SetPressedObject(lv_obj_t* object) {
        if (pressed_object_ == object) return;
        // Enter can delete its own target (unlock, dialogs, page changes).
        // LVGL's widget-tree check follows parent pointers and cannot validate
        // freed memory. Clear this reference during LV_EVENT_DELETE instead.
        if (pressed_object_)
            lv_obj_remove_event_cb_with_user_data(pressed_object_, nullptr, &pressed_object_);
        pressed_object_ = object;
        if (pressed_object_) lv_obj_null_on_delete(&pressed_object_);
    }

    void Emit(uint32_t key, bool pressed) {
        key_ = key;
        pressed_ = pressed;
        SetPressedObject(pressed ? focused() : nullptr);
        lv_indev_read(keypad_);
    }

    bool Contains(lv_obj_t* object) const {
        return std::find(candidates_.begin(), candidates_.begin() + count_, object) !=
               candidates_.begin() + count_;
    }

    void Collect(lv_obj_t* object, lv_obj_t* root) {
        if (!object || lv_obj_is_hidden(object) ||
            lv_obj_has_state(object, LV_STATE_DISABLED)) return;
        bool actionable = lv_obj_is_group_def(object);
        bool handles_key = false;
        for (uint32_t i = 0; i < lv_obj_get_event_count(object); ++i) {
            // LVGL 9.6 exposes callback descriptors but has no filter getter.
            // Inspect the pinned descriptor to avoid focusing drag catchers.
            const auto filter = lv_obj_get_event_dsc(object, i)->filter & ~LV_EVENT_PREPROCESS;
            handles_key |= filter == LV_EVENT_KEY;
            actionable |= filter == LV_EVENT_CLICKED || filter == LV_EVENT_SHORT_CLICKED ||
                          filter == LV_EVENT_VALUE_CHANGED || filter == LV_EVENT_KEY;
        }
        if (lv_obj_is_scrollable(object) && !lv_obj_is_group_def(object) && !handles_key)
            actionable = false;
        // Exclude full-screen scrims and gesture catchers. Their child buttons
        // remain navigable, including controls outside a scroll viewport.
        const bool scrim = lv_obj_get_width(object) >= lv_display_get_horizontal_resolution(display_) * 9 / 10 &&
                           lv_obj_get_height(object) >= lv_display_get_vertical_resolution(display_) * 4 / 5 &&
                           lv_obj_get_child_count(object) != 0 && !lv_obj_is_group_def(object) && !handles_key;
        const auto* owner = lv_obj_get_group(object);
        if (actionable && !scrim && (object != root || lv_obj_get_child_count(object) == 0 ||
                                    lv_obj_is_group_def(object) || handles_key) &&
            lv_obj_is_clickable(object) &&
            lv_obj_get_width(object) > 0 && lv_obj_get_height(object) > 0 &&
            (!owner || owner == group_) && count_ < candidates_.size() && !Contains(object))
            candidates_[count_++] = object;
        for (uint32_t i = 0; i < lv_obj_get_child_count(object); ++i)
            Collect(lv_obj_get_child(object, i), root);
    }

    void Refresh(lv_obj_t* scope, bool enabled, uint32_t now) {
        auto* requested_scope = scope;
        if (enabled && !scope) {
            lv_obj_update_layout(lv_display_get_layer_sys(display_));
            lv_obj_update_layout(lv_display_get_layer_top(display_));
            scope = FindModal(lv_display_get_layer_sys(display_));
            if (!scope) scope = FindModal(lv_display_get_layer_top(display_));
        }
        count_ = 0;
        if (enabled) {
            if (scope) { lv_obj_update_layout(scope); Collect(scope, scope); }
            else {
                for (auto* root : {lv_display_get_screen_active(display_),
                                   lv_display_get_layer_top(display_)}) {
                    lv_obj_update_layout(root);
                    Collect(root, root);
                }
            }
        }
        bool changed = lv_group_get_obj_count(group_) != count_;
        for (uint32_t i = 0; !changed && i < count_; ++i)
            changed = lv_group_get_obj_by_index(group_, i) != candidates_[i];
        auto* previous = lv_group_get_focused(group_);
        if (changed) {
            const bool retain_focus = previous && Contains(previous);
            if (pressed_) { Cancel(); blocked_ = true; }
            // The group automatically removes deleted objects, so every old
            // member dereferenced here is still alive.
            for (uint32_t i = 0; i < lv_group_get_obj_count(group_); ++i)
                FocusStyle(lv_group_get_obj_by_index(group_, i), false);
            lv_group_remove_all_objs(group_);
            // Preserve the viewport by avoiding first-member autofocus and
            // native scroll-on-focus when the previous target survives.
            if (retain_focus) lv_group_focus_freeze(group_, true);
            for (size_t i = 0; i < count_; ++i) {
                FocusStyle(candidates_[i], true);
                lv_group_add_obj(group_, candidates_[i]);
            }
            if (retain_focus) {
                lv_group_focus_freeze(group_, false);
                const bool scroll_on_focus = lv_obj_is_scroll_on_focus(previous);
                lv_obj_set_scroll_on_focus(previous, false);
                lv_group_focus_obj(previous);
                lv_obj_set_scroll_on_focus(previous, scroll_on_focus);
            } else if (count_) Focus(candidates_[0]);
        }
        scope_ = requested_scope;
        enabled_ = enabled;
        refreshed_ = now;
    }

    lv_obj_t* FindModal(lv_obj_t* object) const {
        if (lv_obj_is_hidden(object)) return nullptr;
        for (int32_t i = int32_t(lv_obj_get_child_count(object)) - 1; i >= 0; --i)
            if (auto* modal = FindModal(lv_obj_get_child(object, i))) return modal;
        lv_area_t area;
        lv_obj_get_coords(object, &area);
        // Trusted permission prompts and third-party modal scrims may live
        // outside the reference shell. Confine focus to the top blocking
        // overlay, just as LVGL's pointer hit testing confines a touch.
        if (object != lv_display_get_layer_top(display_) &&
            object != lv_display_get_layer_sys(display_) &&
            lv_obj_is_clickable(object) && !lv_obj_is_group_def(object) &&
            area.x1 <= 0 && area.y1 <= 0 &&
            area.x2 >= lv_display_get_horizontal_resolution(display_) - 1 &&
            area.y2 >= lv_display_get_vertical_resolution(display_) - 1)
            return object;
        return nullptr;
    }

    static lv_obj_t* ScrollOwner(lv_obj_t* object) {
        for (auto* parent = lv_obj_get_parent(object); parent; parent = lv_obj_get_parent(parent))
            if (lv_obj_is_scrollable(parent)) return parent;
        return nullptr;
    }

    void Step(bool forward) {
        if (!count_) return;
        auto* current = lv_group_get_focused(group_);
        size_t index = 0;
        while (index < count_ && candidates_[index] != current) ++index;
        index = forward ? (index + 1) % count_ : (index + count_ - 1) % count_;
        Focus(candidates_[index]);
    }

    void Move(uint32_t key) {
        auto* current = lv_group_get_focused(group_);
        if (!current) return;
        lv_area_t from;
        lv_obj_get_coords(current, &from);
        if (IsMatrix(current)) {
            const auto button = lv_buttonmatrix_get_selected_button(current);
            if (ButtonEnabled(current, button)) from = ButtonArea(current, button);
        }
        lv_obj_t* best = nullptr;
        uint32_t best_button = LV_BUTTONMATRIX_BUTTON_NONE;
        int64_t best_score = std::numeric_limits<int64_t>::max();
        const bool horizontal = key == LV_KEY_LEFT || key == LV_KEY_RIGHT;
        auto consider = [&](lv_obj_t* candidate, const lv_area_t& to, uint32_t button) {
            if (candidate == current && (!IsMatrix(current) ||
                button == lv_buttonmatrix_get_selected_button(current))) return;
            int64_t dx = int64_t(to.x1) + to.x2 - from.x1 - from.x2;
            int64_t dy = int64_t(to.y1) + to.y2 - from.y1 - from.y2;
            if ((key == LV_KEY_LEFT && dx >= 0) || (key == LV_KEY_RIGHT && dx <= 0) ||
                (key == LV_KEY_UP && dy >= 0) || (key == LV_KEY_DOWN && dy <= 0)) return;
            dx = std::abs(dx); dy = std::abs(dy);
            const bool overlap = horizontal ? to.y1 <= from.y2 && to.y2 >= from.y1 :
                                              to.x1 <= from.x2 && to.x2 >= from.x1;
            const bool same_scroll = ScrollOwner(current) == ScrollOwner(candidate);
            const int64_t score = (horizontal ? dx + dy * 4 : dy + dx * 4) +
                (overlap ? 0 : 100000) + (same_scroll ? 0 : 1000000);
            if (score < best_score) { best_score = score; best = candidate; best_button = button; }
        };
        for (size_t i = 0; i < count_; ++i) {
            auto* candidate = candidates_[i];
            if (IsMatrix(candidate)) {
                for (uint32_t button = 0; lv_buttonmatrix_get_button_text(candidate, button); ++button)
                    if (ButtonEnabled(candidate, button)) consider(candidate, ButtonArea(candidate, button), button);
            } else {
                lv_area_t to;
                lv_obj_get_coords(candidate, &to);
                consider(candidate, to, LV_BUTTONMATRIX_BUTTON_NONE);
            }
        }
        if (best) {
            Focus(best);
            if (best_button != LV_BUTTONMATRIX_BUTTON_NONE)
                lv_buttonmatrix_set_selected_button(best, best_button);
        }
    }

    void Focus(lv_obj_t* object) {
        lv_group_focus_obj(object);
        lv_obj_scroll_to_view_recursive(object, LV_ANIM_OFF);
    }

    lv_display_t* display_ = nullptr;
    lv_group_t* group_ = nullptr;
    lv_indev_t* keypad_ = nullptr;
    lv_style_t outline_{}, matrix_outline_{};
    std::array<lv_obj_t*, 256> candidates_{};
    size_t count_ = 0;
    lv_obj_t* scope_ = nullptr;
    lv_obj_t* pressed_object_ = nullptr;
    bool enabled_ = false, pressed_ = false, blocked_ = false;
    uint32_t key_ = LV_KEY_ENTER, held_ = 0, refreshed_ = 0;
    uint32_t repeated_ = 0, repeat_delay_ = 400;
    void* activate_context_ = nullptr;
    void (*activate_text_)(void*, lv_obj_t*) = nullptr;
};
}
