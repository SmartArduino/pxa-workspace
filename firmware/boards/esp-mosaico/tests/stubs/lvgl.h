#pragma once

#include <cstdint>

struct lv_display_t {};
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
inline lv_obj_t* lv_display_get_screen_active(lv_display_t*) { return nullptr; }
inline void lv_obj_invalidate(lv_obj_t*) {}

inline void lv_obj_invalidate_area(lv_obj_t*, const lv_area_t*) {}
