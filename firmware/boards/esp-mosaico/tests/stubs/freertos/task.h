#pragma once

#include "FreeRTOS.h"

inline void xTaskNotifyGive(TaskHandle_t) {}
inline uint32_t ulTaskNotifyTake(int, uint32_t) { return 0; }
inline void vTaskDelay(unsigned) {}
inline int xTaskCreate(void (*)(void*), const char*, unsigned, void*, unsigned,
                       TaskHandle_t* handle) {
    *handle = reinterpret_cast<void*>(1);
    return pdPASS;
}
inline int xTaskCreatePinnedToCore(void (*task)(void*), const char* name,
        unsigned stack, void* context, unsigned priority, TaskHandle_t* handle, int) {
    return xTaskCreate(task, name, stack, context, priority, handle);
}

inline void vTaskDelete(TaskHandle_t) {}
