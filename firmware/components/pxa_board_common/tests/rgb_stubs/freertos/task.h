#pragma once
#include "FreeRTOS.h"
#include <cassert>
inline bool pxa_test_task_create_fail = false;
inline unsigned pxa_test_tasks_deleted = 0;
inline size_t pxa_test_stack_bytes = 0;
inline TaskHandle_t xTaskCreateStatic(void(*)(void*),const char*,size_t bytes,void*,unsigned,StackType_t*,StaticTask_t*) {
    pxa_test_stack_bytes=bytes;
    return pxa_test_task_create_fail ? nullptr : reinterpret_cast<void*>(1);
}
inline void vTaskDelete(TaskHandle_t task) { assert(task); ++pxa_test_tasks_deleted; }
inline void xTaskNotifyGive(TaskHandle_t) {}
inline unsigned ulTaskNotifyTake(int,unsigned) { return 0; }
inline void vTaskDelay(unsigned) {}
