// SPDX-License-Identifier: Apache-2.0
#pragma once
#ifdef ESP_PLATFORM
#include "wels_decoder_thread.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
// Picture rows are manual-reset, single-producer readiness flags. Allocating
// an RTOS mutex/condition pair for every row exhausts the P4's internal SRAM.
// The flags live in the picture's budgeted storage; only the two fixed codec
// workers wait here. Release/acquire publishes the completed row's pixels.
static int pxa_video_wait_row(SWelsDecEvent *event,int32_t timeout) {
    const int64_t start=esp_timer_get_time();
    while(!__atomic_load_n(&event->isSignaled,__ATOMIC_ACQUIRE)) {
        if(!timeout || (timeout>0 && esp_timer_get_time()-start>=int64_t(timeout)*1000))
            return WELS_DEC_THREAD_WAIT_TIMEDOUT;
        vTaskDelay(1);
    }
    return WELS_DEC_THREAD_WAIT_SIGNALED;
}
#endif
