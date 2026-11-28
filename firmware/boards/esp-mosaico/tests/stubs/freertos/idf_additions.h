#pragma once

#include "task.h"
#include <cassert>

inline constexpr unsigned MALLOC_CAP_8BIT = 1u << 2;
inline constexpr unsigned MALLOC_CAP_SPIRAM = 1u << 10;

inline int xTaskCreatePinnedToCoreWithCaps(void (*task)(void*), const char* name,
        unsigned stack, void* context, unsigned priority, TaskHandle_t* handle,
        int core, unsigned caps) {
    assert(caps == (MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM));
    return xTaskCreatePinnedToCore(task, name, stack, context, priority, handle, core);
}

inline void vTaskDeleteWithCaps(TaskHandle_t handle) { vTaskDelete(handle); }
