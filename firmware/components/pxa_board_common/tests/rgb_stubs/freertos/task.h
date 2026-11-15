#pragma once
#include "FreeRTOS.h"
#include <cassert>
inline bool pxa_test_task_create_fail = false;
inline unsigned pxa_test_tasks_deleted = 0;
inline size_t pxa_test_stack_bytes = 0;
inline int pxa_test_current_core = 1, pxa_test_pinned_core = -1;
inline int xPortGetCoreID() { return pxa_test_current_core; }
inline TaskHandle_t xTaskCreateStatic(void(*)(void*),const char*,size_t bytes,void*,unsigned,StackType_t*,StaticTask_t*) {
    pxa_test_stack_bytes=bytes;
    return pxa_test_task_create_fail ? nullptr : reinterpret_cast<void*>(1);
}
inline void vTaskDelete(TaskHandle_t task) { assert(task); ++pxa_test_tasks_deleted; }
inline void xTaskNotifyGive(TaskHandle_t) {}
inline unsigned ulTaskNotifyTake(int,unsigned) { return 0; }
inline void vTaskDelay(unsigned) {}

inline TaskHandle_t xTaskCreateStaticPinnedToCore(void(*entry)(void*),const char* name,size_t bytes,
        void* context,unsigned priority,StackType_t* stack,StaticTask_t* task,int core) {
    pxa_test_pinned_core=core;
    return xTaskCreateStatic(entry,name,bytes,context,priority,stack,task);
}
