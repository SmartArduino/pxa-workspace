#pragma once

#include <cstdint>

struct lv_display_t { uint32_t inv_p = 0; };
struct lv_obj_t {};
inline void lv_lock() {}
inline void lv_unlock() {}
struct lv_area_t {
    int32_t x1;
    int32_t y1;
    int32_t x2;
    int32_t y2;
};

inline int32_t lv_area_get_width(const lv_area_t* area) {
    return area->x2 - area->x1 + 1;
}
inline int32_t lv_area_get_height(const lv_area_t* area) {
    return area->y2 - area->y1 + 1;
}
inline uint32_t lv_area_get_size(const lv_area_t* area) {
    return lv_area_get_width(area) * lv_area_get_height(area);
}
inline lv_obj_t* lv_display_get_screen_active(lv_display_t*) { return nullptr; }
inline lv_obj_t* lv_display_get_screen_prev(lv_display_t*) { return nullptr; }
inline lv_obj_t* lv_display_get_layer_bottom(lv_display_t*) { return nullptr; }
inline lv_obj_t* lv_display_get_layer_top(lv_display_t*) { return nullptr; }
inline lv_obj_t* lv_display_get_layer_sys(lv_display_t*) { return nullptr; }
inline void lv_obj_update_layout(lv_obj_t*) {}
inline bool lv_display_is_invalidation_enabled(lv_display_t*) { return true; }
inline uint16_t lv_anim_count_running() { return 0; }
inline void lv_obj_invalidate(lv_obj_t*) {}

inline void lv_obj_invalidate_area(lv_obj_t*, const lv_area_t*) {}
