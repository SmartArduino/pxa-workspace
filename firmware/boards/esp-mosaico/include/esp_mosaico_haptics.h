#pragma once
#include <cstdint>
#include <esp_err.h>

bool EspMosaicoInitializeHaptics();
// 0 stops immediately; maximum one second per request. Task context only.
esp_err_t EspMosaicoVibrate(uint32_t duration_ms);
