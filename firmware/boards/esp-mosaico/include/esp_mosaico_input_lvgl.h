#pragma once

#include <lvgl.h>

// LVGL's recognizer helper also replaces data->state using its current contact
// set, which can be newer than the queued sample (including its final UP).
// Keep gesture metadata, but let the endpoint queue own pointer state.
inline void MosaicoSetGestureMetadata(lv_indev_t* input, lv_indev_data_t* data) {
#if LV_USE_GESTURE_RECOGNITION
    const auto state = data->state;
    lv_indev_gesture_recognizers_set_data(input, data);
    data->state = state;
#else
    (void)input;
    (void)data;
#endif
}
