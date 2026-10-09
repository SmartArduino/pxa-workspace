#pragma once

inline constexpr int ESP_OK = 0;
inline int esp_lv_adapter_lock(int) { return ESP_OK; }
inline void esp_lv_adapter_unlock() {}
