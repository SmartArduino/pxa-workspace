#pragma once
template <typename... Args> inline void BatteryTestLog(const char*, const char*, Args...) {}
#define ESP_LOGI BatteryTestLog
#define ESP_LOGW BatteryTestLog
