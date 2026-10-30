#pragma once
#include <cstdint>
#include <mutex>
using StackType_t = uint32_t;
using TaskHandle_t = void*;
using StaticTask_t = int;
struct StaticSemaphore_t { std::mutex mutex; };
using SemaphoreHandle_t = std::mutex*;
#define pdTRUE 1
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(x) (x)
