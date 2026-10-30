#ifdef PXA_TEST_GAMEKIT_1
#include "doit_gamekit_1_navigation.h"
#else
#include "doit_gamekit_navigation.h"
#endif
#include <cassert>
#include <cstdio>

#ifdef PXA_TEST_GAMEKIT_1
using namespace doit_gamekit_1;
#else
using namespace doit_gamekit;
#endif
static uint32_t tick;
static unsigned clicks, releases, typed;
alignas(64) static uint32_t frame[320 * 240];
static void AssertVisibleFocus(lv_display_t* display, lv_obj_t* object) {
    lv_refr_now(display);
    lv_area_t area;
    lv_obj_get_coords(object, &area);
    assert(area.x1 >= 0 && area.y1 >= 0 && area.x2 < 320 && area.y2 < 240);
    const int x = (area.x1 + area.x2) / 2;
    const int y = (area.y1 + area.y2) / 2;
    // Sample rendered pixels, including the edges that the parent clips.
    // This catches both theme precedence and an outline outside the widget.
    for (const auto index : {area.y1 * 320 + x, area.y2 * 320 + x,
                             y * 320 + area.x1, y * 320 + area.x2})
        assert((frame[index] & 0xffffff) == 0xffb74d);
}
static void Update(KeyNavigation& navigation, uint32_t buttons = 0,
                   lv_obj_t* scope = nullptr, bool enabled = true, unsigned elapsed = 20) {
    tick += elapsed;
    lv_tick_inc(elapsed);
    navigation.Update(scope, enabled, buttons, tick);
}
static void Tap(KeyNavigation& navigation, uint32_t buttons, lv_obj_t* scope = nullptr) {
    Update(navigation, buttons, scope);
    Update(navigation, 0, scope);
}
static void Plain(lv_obj_t* object) {
    lv_obj_remove_style_all(object);
    lv_obj_set_scrollable(object, false);
}
static lv_obj_t* MakeButton(lv_obj_t* parent, int x, int y, bool plain = false) {
    auto* object = plain ? lv_obj_create(parent) : lv_button_create(parent);
    Plain(object);
    lv_obj_set_clickable(object, true);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, 70, 30);
    lv_obj_add_event_cb(object, [](lv_event_t*) { ++clicks; }, LV_EVENT_CLICKED, nullptr);
    return object;
}
static void SelectControl(KeyNavigation& navigation, lv_obj_t* object, lv_obj_t* scope = nullptr) {
    for (int i = 0; i < 30 && navigation.focused() != object; ++i) Tap(navigation, R, scope);
    assert(navigation.focused() == object);
}

int main() {
    lv_init();
    auto* display = lv_display_create(320, 240);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_XRGB8888);
    lv_display_set_buffers(display, frame, nullptr, sizeof(frame), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display, [](lv_display_t* output, const lv_area_t*, uint8_t*) {
        lv_display_flush_ready(output);
    });
    auto* screen = lv_display_get_screen_active(display);
    Plain(screen);
    // The real board keeps both a fallback pointer and a remote pointer. LVGL
    // must not treat programmatic keypad focus as mouse focus in this setup.
    auto* pointer = lv_indev_create();
    lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_mode(pointer, LV_INDEV_MODE_EVENT);
    KeyNavigation navigation;
    assert(navigation.Initialize(display));
    auto* first = MakeButton(screen, 10, 10);
    auto* right = MakeButton(screen, 100, 10);
    auto* below = MakeButton(screen, 10, 60, true); // Settings uses ordinary clickable rows.
    auto* hidden = MakeButton(screen, 10, 45);
    lv_obj_set_hidden(hidden, true);
    auto* disabled = MakeButton(screen, 10, 40);
    lv_obj_add_state(disabled, LV_STATE_DISABLED);
    Update(navigation);
    assert(navigation.focused() == first);
#ifdef PXA_TEST_GAMEKIT_1
    // Home restores bookmarks through the discovered keypad group before
    // Update runs. Touch mode must not repeatedly create a default blue focus
    // and immediately discard it. Re-enabling the pad restores its selection.
    auto discover_group = [&]() -> lv_group_t* {
        for (auto* device = lv_indev_get_next(nullptr); device;
             device = lv_indev_get_next(device))
            if (lv_indev_get_type(device) == LV_INDEV_TYPE_KEYPAD &&
                lv_indev_get_display(device) == display)
                return lv_indev_get_group(device);
        return nullptr;
    };
    unsigned focus_events = 0;
    lv_obj_add_event_cb(first, [](lv_event_t* event) {
        ++*static_cast<unsigned*>(lv_event_get_user_data(event));
    }, LV_EVENT_FOCUSED, &focus_events);
    navigation.SetInputActive(false);
    Update(navigation, 0, nullptr, false);
    for (unsigned i = 0; i < 20; ++i) {
        if (auto* group = discover_group(); group && !lv_group_get_obj_count(group))
            lv_group_add_obj(group, first);
        Update(navigation, 0, nullptr, false, 60);
        lv_refr_now(display);
        assert(!lv_obj_has_state(first, LV_STATE_FOCUSED));
    }
    assert(focus_events == 0);
    navigation.SetInputActive(true);
    auto* restored_group = discover_group();
    assert(restored_group);
    lv_group_add_obj(restored_group, first);
    Update(navigation);
    assert(navigation.focused() == first && focus_events >= 1 && focus_events <= 2);
    AssertVisibleFocus(display, first);
    const unsigned restored_events = focus_events;
    for (unsigned i = 0; i < 20; ++i) Update(navigation, 0, nullptr, true, 60);
    assert(focus_events == restored_events);
#endif
    Tap(navigation, Right);
    assert(navigation.focused() == right);
    Tap(navigation, Left);
    Tap(navigation, Down);
    assert(navigation.focused() == below);
    unsigned deletions = 0;
    lv_obj_add_event_cb(below, [](lv_event_t* event) {
        if (lv_event_get_key(event) == LV_KEY_DEL)
            ++*static_cast<unsigned*>(lv_event_get_user_data(event));
    }, LV_EVENT_KEY, &deletions);
    Update(navigation, X);
    Update(navigation, X);
    assert(deletions == 1);
    Update(navigation, 0);
    Tap(navigation, A);
    assert(clicks == 1);

    // Real LVGL Enter is canceled when a modal replaces the focus scope.
    Update(navigation, A);
    auto* modal = lv_obj_create(screen);
    Plain(modal);
    lv_obj_set_size(modal, 320, 240);
    auto* confirm = MakeButton(modal, 50, 80);
    Update(navigation, A, modal);
    assert(navigation.focused() == confirm);
    Update(navigation, 0, modal);
    assert(clicks == 1);
    Tap(navigation, A, modal);
    assert(clicks == 2);
    lv_obj_delete(modal);
    Update(navigation);
    assert(navigation.focused() != confirm);
    auto* external_modal = lv_obj_create(lv_layer_top());
    Plain(external_modal);
    lv_obj_set_size(external_modal, 320, 240);
    lv_obj_set_clickable(external_modal, true);
    auto* allow = MakeButton(external_modal, 40, 80);
    Update(navigation, 0, nullptr, true, 60);
    assert(navigation.focused() == allow); // No shell scope is needed for a permission scrim.
    lv_obj_delete(external_modal);

    // Focus scrolls to an off-screen item instead of getting trapped by the
    // navigation bar, even while its buttons are closer in screen pixels.
    lv_obj_clean(screen);
    auto* list = lv_obj_create(screen);
    Plain(list);
    lv_obj_set_scrollable(list, true);
    lv_obj_add_event_cb(list, [](lv_event_t*) { ++clicks; }, LV_EVENT_CLICKED, nullptr);
    lv_obj_set_size(list, 250, 150);
    auto* row1 = MakeButton(list, 10, 10);
    auto* row2 = MakeButton(list, 10, 120);
    auto* row3 = MakeButton(list, 10, 220);
    (void)MakeButton(screen, 10, 180);
    Update(navigation, 0, nullptr, true, 60);
    assert(navigation.focused() == row1);
    Tap(navigation, Down);
    assert(navigation.focused() == row2);
    Tap(navigation, Down);
    assert(navigation.focused() == row3);
    assert(lv_obj_get_scroll_y(list) > 0);

    // Recollecting controls must preserve a manually restored viewport when
    // the focused target survives, including native widgets that scroll on focus.
    for (const bool plain : {false, true}) {
        lv_obj_clean(screen);
        auto* viewport = lv_obj_create(screen);
        Plain(viewport);
        lv_obj_set_scrollable(viewport, true);
        lv_obj_set_size(viewport, 250, 150);
        (void)MakeButton(viewport, 10, 10, plain);
        auto* surviving = MakeButton(viewport, 10, 220, plain);
        (void)MakeButton(viewport, 10, 400, plain);
        Update(navigation, 0, nullptr, true, 60);
        SelectControl(navigation, surviving);
        auto* group = lv_indev_get_group(navigation.keypad());
        assert(lv_group_get_obj_count(group) == 3);
        (void)MakeButton(viewport, 100, 320, plain);
        lv_obj_update_layout(viewport);
        lv_obj_scroll_to_y(viewport, 40, LV_ANIM_OFF);
        Update(navigation, 0, nullptr, true, 60);
        assert(lv_group_get_obj_count(group) == 4);
        assert(navigation.focused() == surviving);
        assert(lv_obj_get_scroll_y(viewport) == 40);
        tick += 500;
        lv_tick_inc(500);
        lv_timer_handler(); // Also catch animated scroll from transient focus.
        assert(lv_obj_get_scroll_y(viewport) == 40);

        // A rebuilt settings page first resolves a new object, adds it to the
        // existing keypad group, focuses it, then restores the exact offset.
        lv_obj_clean(viewport);
        assert(lv_group_get_obj_count(group) == 0);
        auto* fallback = MakeButton(viewport, 10, 10, plain);
        auto* restored = MakeButton(viewport, 10, 220, plain);
        (void)MakeButton(viewport, 10, 400, plain);
        lv_obj_update_layout(viewport);
        lv_group_add_obj(group, restored);
        lv_group_focus_obj(restored);
        lv_obj_scroll_to_y(viewport, 40, LV_ANIM_OFF);
        Update(navigation, 0, nullptr, true, 60);
        assert(lv_group_get_obj_count(group) == 3);
        assert(lv_group_get_obj_by_index(group, 0) == fallback);
        assert(navigation.focused() == restored);
        assert(lv_obj_get_scroll_y(viewport) == 40);
        tick += 500;
        lv_tick_inc(500);
        lv_timer_handler();
        assert(lv_obj_get_scroll_y(viewport) == 40);

        // A previous target absent from collection still reveals the first row.
        lv_obj_add_state(restored, LV_STATE_DISABLED);
        assert(navigation.focused() == restored);
        lv_obj_scroll_to_y(viewport, 250, LV_ANIM_OFF);
        assert(lv_obj_get_scroll_y(viewport) == 250);
        Update(navigation, 0, nullptr, true, 60);
        assert(lv_group_get_obj_count(group) == 2);
        assert(navigation.focused() == fallback);
        assert(lv_obj_get_scroll_y(viewport) < 250);
        lv_area_t viewport_area, fallback_area;
        lv_obj_get_coords(viewport, &viewport_area);
        lv_obj_get_coords(fallback, &fallback_area);
        assert(fallback_area.y1 >= viewport_area.y1 &&
               fallback_area.y2 <= viewport_area.y2);
    }

    // Native controls retain their semantics; L/R can leave them.
    lv_obj_clean(screen);
    auto* slider = lv_slider_create(screen);
    lv_obj_set_pos(slider, 10, 10);
    lv_obj_set_size(slider, 100, 12);
    lv_slider_set_value(slider, 50, LV_ANIM_OFF);
    lv_obj_add_event_cb(slider, [](lv_event_t*) { ++releases; }, LV_EVENT_RELEASED, nullptr);
    auto* toggle = lv_switch_create(screen);
    lv_obj_set_pos(toggle, 10, 50);
    auto* dropdown = lv_dropdown_create(screen);
    lv_obj_set_pos(dropdown, 10, 100);
    lv_dropdown_set_options(dropdown, "first\nsecond\nthird");
    auto* matrix = lv_buttonmatrix_create(screen);
    lv_obj_set_pos(matrix, 10, 150);
    lv_obj_set_size(matrix, 200, 30);
    static const char* map[] = {"A", "B", "C", ""};
    lv_buttonmatrix_set_map(matrix, map);
    lv_buttonmatrix_set_button_ctrl_all(matrix, LV_BUTTONMATRIX_CTRL_CLICK_TRIG);
    lv_obj_add_event_cb(matrix, [](lv_event_t*) { ++typed; }, LV_EVENT_VALUE_CHANGED, nullptr);
    Update(navigation, 0, nullptr, true, 60);
    SelectControl(navigation, slider);
    Tap(navigation, Right);
    assert(lv_slider_get_value(slider) > 50 && releases == 1);
    Tap(navigation, R);
    assert(navigation.focused() == toggle);
    Tap(navigation, A);
    assert(lv_obj_has_state(toggle, LV_STATE_CHECKED));
    SelectControl(navigation, dropdown);
    Tap(navigation, A);
    assert(lv_dropdown_is_open(dropdown));
    Tap(navigation, Down);
    Tap(navigation, A);
    assert(lv_dropdown_get_selected(dropdown) == 1 && !lv_dropdown_is_open(dropdown));
    Tap(navigation, A);
    assert(navigation.ClosePopup() && !lv_dropdown_is_open(dropdown));
    SelectControl(navigation, matrix);
    const auto selected = lv_buttonmatrix_get_selected_button(matrix);
    assert(selected != LV_BUTTONMATRIX_BUTTON_NONE);
    Tap(navigation, A);
    assert(typed == 1); // The first A types without a prerequisite arrow press.
    Tap(navigation, Right);
    assert(lv_buttonmatrix_get_selected_button(matrix) != selected);

    // Unlock deletes the focused lock screen inside its own Enter event.
    // The board resets its pointer there and cancels keypad input next poll.
    for (unsigned iteration = 0; iteration < 100; ++iteration) {
        lv_obj_clean(screen);
        (void)MakeButton(screen, 10, 10);
        auto* lock = lv_obj_create(lv_layer_top());
        Plain(lock); lv_obj_set_size(lock, 320, 240); lv_obj_set_clickable(lock, true);
        lv_obj_add_event_cb(lock, [](lv_event_t* event) {
            if (lv_event_get_key(event) == LV_KEY_ENTER) {
                auto* pointer = static_cast<lv_indev_t*>(lv_event_get_user_data(event));
                lv_obj_delete(lv_event_get_target_obj(event));
                lv_indev_reset(pointer, nullptr);
            }
        }, LV_EVENT_KEY, pointer);
        Update(navigation, 0, lock, true, 60);
        assert(navigation.focused() == lock);
        Update(navigation, A, lock);
        navigation.Cancel();
        // Desktop retains focus navigation; a foreground game returns to
        // controller mode. Both must release the deleted Enter target safely.
        Update(navigation, 0, nullptr, iteration % 2 == 0);
    }

    // Releasing A must remove the previous target's delete observer. Deleting
    // that old object while another is held must not cancel the new target.
    lv_obj_clean(screen);
    auto* old_target = MakeButton(screen, 10, 10);
    auto* new_target = MakeButton(screen, 10, 50);
    unsigned lost = 0;
    lv_obj_add_event_cb(new_target, [](lv_event_t* event) {
        ++*static_cast<unsigned*>(lv_event_get_user_data(event));
    }, LV_EVENT_PRESS_LOST, &lost);
    Update(navigation, 0, nullptr, true, 60);
    const unsigned event_count = lv_obj_get_event_count(old_target);
    for (unsigned repeat = 0; repeat < 20; ++repeat) {
        Update(navigation, A);
        assert(lv_obj_get_event_count(old_target) == event_count + 1);
    }
    Update(navigation, 0);
    assert(lv_obj_get_event_count(old_target) == event_count);
    Tap(navigation, Down);
    assert(navigation.focused() == new_target);
    Update(navigation, A);
    lv_obj_delete(old_target);
    navigation.Cancel();
    assert(lost == 1);

    // A text field can also disappear in its Enter callback. Do not activate
    // the editor after that callback has destroyed its target.
    lv_obj_clean(screen);
    auto* text = lv_textarea_create(screen);
    unsigned activations = 0;
    navigation.SetTextActivation(&activations, [](void* context, lv_obj_t*) {
        ++*static_cast<unsigned*>(context);
    });
    lv_obj_add_event_cb(text, [](lv_event_t* event) {
        if (lv_event_get_key(event) == LV_KEY_ENTER)
            lv_obj_delete(lv_event_get_target_obj(event));
    }, LV_EVENT_KEY, nullptr);
    Update(navigation, 0, nullptr, true, 60);
    Update(navigation, A);
    navigation.Cancel();
    assert(activations == 0);
    navigation.SetTextActivation(nullptr, nullptr);
    const unsigned clicks_before_removal = clicks;

    // Removing a focused object while Enter is held must not click its successor.
    lv_obj_clean(screen);
    auto* removed = MakeButton(screen, 10, 10);
    (void)MakeButton(screen, 10, 50);
    Update(navigation, 0, nullptr, true, 60);
    assert(navigation.focused() == removed);
    Update(navigation, A);
    lv_obj_delete(removed);
    Update(navigation, A);
    Update(navigation);
    assert(clicks == clicks_before_removal);

    // A control can touch all four edges of its clipping parent, as desktop
    // tiles and settings rows do. Test both the native theme and plain rows.
    lv_obj_clean(screen);
    auto* viewport = lv_obj_create(screen);
    Plain(viewport);
    lv_obj_set_pos(viewport, 80, 40);
    lv_obj_set_size(viewport, 100, 40);
    auto* edge_button = lv_button_create(viewport);
    lv_obj_set_pos(edge_button, 0, 0);
    lv_obj_set_size(edge_button, 100, 40);
    Update(navigation, 0, nullptr, true, 60);
    assert(navigation.focused() == edge_button);
    AssertVisibleFocus(display, edge_button);
    lv_obj_delete(edge_button);
    auto* edge_row = MakeButton(viewport, 0, 0, true);
    lv_obj_set_size(edge_row, 100, 40);
    Update(navigation, 0, nullptr, true, 60);
    assert(navigation.focused() == edge_row);
    AssertVisibleFocus(display, edge_row);
    Update(navigation, 0, nullptr, false); // Mouse/game mode releases the focus group.
    assert(navigation.focused() == nullptr);
    navigation.Deinitialize();
    lv_indev_delete(pointer);
    lv_deinit();
    std::puts("DOIT GameKit real LVGL focus navigation tests passed");
}
