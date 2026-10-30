#pragma once
#include "FreeRTOS.h"
inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t* s) { return &s->mutex; }
inline int xSemaphoreTake(SemaphoreHandle_t s, unsigned wait) {
    if (!wait) return s->try_lock();
    s->lock(); return 1;
}
inline void xSemaphoreGive(SemaphoreHandle_t s) { s->unlock(); }
