// SPDX-License-Identifier: Apache-2.0
#include "pxa_esp_storage_worker.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"

typedef struct { pxa_storage_work_fn work; void *context; } storage_work_t;
/* Board-lifetime worker: its handle and stack never race task deletion. One
 * activation owns this FIFO; end's barrier retires all borrowed pointers. */
static QueueHandle_t g_queue;
static SemaphoreHandle_t g_drained;
static TaskHandle_t g_worker;
static void (*g_notify)(void *);
static void *g_notify_context;
static int g_active;

static void storage_worker(void *unused) {
    storage_work_t job;
    (void)unused;
    for (;;) {
        if (xQueueReceive(g_queue, &job, portMAX_DELAY) != pdTRUE) continue;
        if (!job.work) {
            xSemaphoreGive(g_drained);
            continue;
        }
        const int64_t started = esp_timer_get_time();
        job.work(job.context);
        const int64_t elapsed = esp_timer_get_time() - started;
        if (elapsed >= 8000)
            ESP_LOGI("PxaStorage", "KV backend io_us=%llu (worker)",
                (unsigned long long)elapsed);
        if (g_notify) g_notify(g_notify_context);
    }
}

pxa_status_t pxa_esp_storage_worker_begin(void (*notify)(void *), void *context) {
    if (g_active) return PXA_STATUS_BAD_STATE;
    if (!notify) return PXA_STATUS_INVALID_ARGUMENT;
    if (!g_queue) g_queue = xQueueCreate(4, sizeof(storage_work_t));
    if (!g_drained) g_drained = xSemaphoreCreateBinary();
    if (!g_queue || !g_drained) return PXA_STATUS_RESOURCE_LIMIT;
    if (!g_worker && xTaskCreate(storage_worker, "pxa-kv", 6144, NULL, 2,
                                &g_worker) != pdPASS)
        return PXA_STATUS_RESOURCE_LIMIT;
    g_notify = notify;
    g_notify_context = context;
    g_active = 1;
    return PXA_STATUS_OK;
}

pxa_status_t pxa_esp_storage_worker_submit(void *context,
    pxa_storage_work_fn work, void *work_context) {
    storage_work_t job = {work, work_context};
    (void)context;
    if (!g_active || !work) return PXA_STATUS_BAD_STATE;
    return xQueueSend(g_queue, &job, 0) == pdTRUE ?
        PXA_STATUS_OK : PXA_STATUS_WOULD_BLOCK;
}

void pxa_esp_storage_worker_end(void) {
    storage_work_t barrier = {NULL, NULL};
    if (!g_active) return;
    g_active = 0;
    /* FIFO admission is serialized by the runtime owner. No producer can
     * enqueue after this barrier, and every accepted backend call finishes. */
    xQueueSend(g_queue, &barrier, portMAX_DELAY);
    xSemaphoreTake(g_drained, portMAX_DELAY);
    g_notify = NULL;
    g_notify_context = NULL;
}
