#pragma once
#include "FreeRTOS.h"
inline TaskHandle_t xTaskCreateStatic(void(*)(void*),const char*,size_t,void*,unsigned,StackType_t*,StaticTask_t*) { return reinterpret_cast<void*>(1); }
inline void xTaskNotifyGive(TaskHandle_t) {}
inline unsigned ulTaskNotifyTake(int,unsigned) { return 0; }
inline void vTaskDelay(unsigned) {}
