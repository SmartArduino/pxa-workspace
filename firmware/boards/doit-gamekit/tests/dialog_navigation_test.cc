#ifdef PXA_TEST_GAMEKIT_1
#include "doit_gamekit_1_navigation.h"
#include "doit_gamekit_1_input.h"
using namespace doit_gamekit_1;
#else
#include "doit_gamekit_navigation.h"
#include "doit_gamekit_input.h"
using namespace doit_gamekit;
#endif
#include <cassert>
#include <cstdio>
extern "C" {
lv_obj_t* test_dialog_show(bool unresponsive, uint32_t id);
void test_dialog_recover(uint32_t id);
extern unsigned dialog_decisions;
extern uint32_t dialog_id;
extern bool dialog_accepted;
}
static void Update(KeyNavigation& navigation, uint32_t buttons = 0,
                   lv_obj_t* fallback = nullptr) {
    lv_tick_inc(60);
    auto* scope = navigation.ResolveScope(fallback);
    // A subscribed Canvas app has no Surface; a stalled Surface also cannot
    // update the presenter's old suppression flag. Route using the live scope.
    bool enabled = NativeNavigation(false, scope != nullptr, true, false);
    navigation.Update(scope, enabled, buttons, lv_tick_get());
}
static void Tap(KeyNavigation& navigation, uint32_t buttons, lv_obj_t* fallback = nullptr) {
    Update(navigation, buttons, fallback);
    Update(navigation, 0, fallback);
}
static lv_obj_t* Mask(lv_obj_t* parent) {
    auto* mask = lv_obj_create(parent);
    lv_obj_remove_style_all(mask);
    lv_obj_set_size(mask, LV_PCT(100), LV_PCT(100));
    lv_obj_set_scrollable(mask, false);
    return mask;
}
int main() {
    lv_init();
    auto* display = lv_display_create(320, 240);
    KeyNavigation navigation;
    assert(navigation.Initialize(display));
    auto* shell = Mask(lv_layer_top());
    auto* underlying = lv_button_create(shell);
    lv_obj_set_size(underlying, 60, 30);
    Update(navigation, 0, shell);
    assert(navigation.focused() == underlying);
    unsigned baseline = 0;
    for (const bool anr : {false, true}) {
        for (const bool accept : {false, true}) {
            Update(navigation, A, shell);
            auto* dialog = test_dialog_show(anr, baseline + 1);
            assert(navigation.ResolveScope(shell) == dialog);
            Update(navigation, A, shell);
            Update(navigation, 0, shell);
            assert(dialog_decisions == baseline); // No held-A click-through.
            auto* left = navigation.focused();
            assert(left && lv_obj_get_parent(lv_obj_get_parent(left)) == dialog);
            Tap(navigation, Right, shell);
            auto* right = navigation.focused();
            assert(right != left);
            Tap(navigation, Left, shell);
            assert(navigation.focused() == left);
            if (accept) Tap(navigation, Right, shell);
            Tap(navigation, A, shell);
            assert(dialog_decisions == ++baseline && dialog_id == baseline);
            assert(dialog_accepted == accept);
            Update(navigation, 0, shell);
            assert(navigation.focused() == underlying);
            lv_timer_handler(); // Delete asynchronous dialog roots under ASan.
        }
    }
    auto* dialog = test_dialog_show(true, 10);
    auto* lock = Mask(lv_layer_top()); // Later lock screen stays above the ANR.
    assert(navigation.ResolveScope(lock) == lock);
    lv_obj_set_hidden(lock, true);
    assert(navigation.ResolveScope(shell) == dialog);
    auto* system = Mask(lv_layer_sys());
    assert(navigation.ResolveScope(dialog) == system);
    lv_obj_delete(system);
    lv_obj_delete(lock);
    Update(navigation, A, shell);
    test_dialog_recover(9); // A stale asynchronous dismissal cannot close ID 10.
    assert(navigation.ResolveScope(shell) == dialog);
    test_dialog_recover(10);
    Update(navigation, A, shell);
    Update(navigation, 0, shell);
    assert(dialog_decisions == baseline); // Recovery is not an operator Stop.
    lv_timer_handler();
    lv_obj_delete(shell);
    Update(navigation);
    assert(!navigation.focused()); // Return the pad to the guest after dismissal.
    navigation.Deinitialize();
    lv_deinit();
    std::puts("Real permission and ANR dialogs: both decisions, held-key cancellation, routing and stacking passed");
}
