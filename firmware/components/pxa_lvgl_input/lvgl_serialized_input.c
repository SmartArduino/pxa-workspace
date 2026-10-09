#include "lvgl.h"

void __real_lv_indev_read(lv_indev_t *indev);

void __wrap_lv_indev_read(lv_indev_t *indev) {
    /* esp_lvgl_port's event wake path reads input under its display mutex,
     * outside lv_timer_handler. PXA's Host uses LVGL's own mutex. Both must
     * serialize event dispatch: LVGL's event_head is a global stack of
     * pointers into the dispatching task's stack, not thread-local state.
     * Timer reads already hold this recursive mutex; wake reads now do too.
     * Keep the managed port dependency intact and allocate no extra state. */
    lv_lock();
    __real_lv_indev_read(indev);
    lv_unlock();
}
