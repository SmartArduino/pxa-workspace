#pragma once
#include "FreeRTOS.h"
inline void xTaskNotifyGive(TaskHandle_t) {}
inline uint32_t ulTaskNotifyTake(int, uint32_t) { return 0; }
inline int xTaskCreatePinnedToCore(void (*)(void*), const char*, unsigned,
                                 void*, unsigned, TaskHandle_t* handle, int) {
    *handle = reinterpret_cast<void*>(1);
    return pdPASS;
}
