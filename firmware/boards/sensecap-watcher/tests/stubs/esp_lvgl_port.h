#pragma once
#include <cstdint>
inline bool lvgl_port_lock(uint32_t) { return true; }
inline void lvgl_port_unlock() {}
