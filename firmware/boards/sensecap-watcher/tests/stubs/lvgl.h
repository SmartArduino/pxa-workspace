#pragma once
#include <cstdint>
using lv_coord_t = int32_t;
struct lv_display_t {};
struct lv_obj_t {};
struct lv_area_t { int32_t x1, y1, x2, y2; };
inline int32_t lv_area_get_width(const lv_area_t* a) { return a->x2-a->x1+1; }
inline int32_t lv_area_get_height(const lv_area_t* a) { return a->y2-a->y1+1; }
inline lv_obj_t* lv_display_get_screen_active(lv_display_t*) { return nullptr; }
inline void lv_obj_invalidate(lv_obj_t*) {}
inline void lv_obj_invalidate_area(lv_obj_t*, const lv_area_t*) {}
inline void lv_lock() {}
inline void lv_unlock() {}
