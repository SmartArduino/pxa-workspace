#pragma once
#include <cstdint>
using TaskHandle_t = void*;
using UBaseType_t = unsigned;
inline constexpr int pdTRUE = 1, pdPASS = 1;
inline constexpr uint32_t portMAX_DELAY = UINT32_MAX;
#define portYIELD_FROM_ISR() ((void)0)
