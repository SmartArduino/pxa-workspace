#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "pxa/raster.h"
#include "pxa/raster_compute.h"

#define CONFIG_PXA_RASTER_WORKER_AFFINITY 0
#define CONFIG_PXA_RASTER_WORKER_STACK_SIZE 4096
#define CONFIG_PXA_RASTER_WORKER_PRIORITY 4
#define EXT_RAM_BSS_ATTR
#define pdTRUE 1
#define portMAX_DELAY UINT32_MAX
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)

typedef uint8_t StackType_t;
typedef sem_t StaticSemaphore_t;
typedef sem_t *SemaphoreHandle_t;
typedef struct {
    pthread_t thread;
    sem_t notification;
    void (*entry)(void *);
    void *argument;
    unsigned core;
    atomic_int enabled;
} StaticTask_t;
typedef StaticTask_t *TaskHandle_t;
static _Thread_local unsigned calling_core;
static _Thread_local TaskHandle_t current_task;
static atomic_uint executions[2];
static unsigned expected_core;
static unsigned created;

static int xPortGetCoreID(void) { return (int)calling_core; }
static uint64_t esp_timer_get_time(void) {
    struct timespec now;
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return (uint64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
}
static uint32_t latency_us(uint64_t begin, uint64_t end) {
    return (uint32_t)(end - begin);
}
static SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *s) {
    assert(sem_init(s, 0, 0) == 0);
    return s;
}
static int xSemaphoreTake(SemaphoreHandle_t s, unsigned timeout) {
    int result;
    do { result = timeout ? sem_wait(s) : sem_trywait(s); }
    while (result && errno == EINTR);
    assert(!result || (!timeout && errno == EAGAIN));
    return result == 0;
}
static void xSemaphoreGive(SemaphoreHandle_t s) {
    assert(sem_post(s) == 0);
}
static void ulTaskNotifyTake(int clear, unsigned timeout) {
    assert(clear && timeout == portMAX_DELAY);
    int result;
    do { result = sem_wait(&current_task->notification); }
    while (result && errno == EINTR);
    assert(result == 0);
    while (sem_trywait(&current_task->notification) == 0) {}
    assert(errno == EAGAIN);
    while (!atomic_load(&current_task->enabled)) sched_yield();
}
static void xTaskNotifyGive(TaskHandle_t task) {
    assert(sem_post(&task->notification) == 0);
}
static void *task_main(void *context) {
    current_task = context;
    calling_core = current_task->core;
    current_task->entry(current_task->argument);
    return NULL;
}
static TaskHandle_t xTaskCreateStaticPinnedToCore(void (*entry)(void *),
    const char *name, unsigned size, void *argument, unsigned priority,
    StackType_t *stack, StaticTask_t *task, unsigned core) {
    assert(name && size == 4096 && priority == 4 && stack && core < 2);
    task->entry = entry;
    task->argument = argument;
    task->core = core;
    atomic_init(&task->enabled, 1);
    assert(sem_init(&task->notification, 0, 0) == 0);
    assert(pthread_create(&task->thread, NULL, task_main, task) == 0);
    ++created;
    return task;
}

/* Each mock kernel writes its disjoint range exactly once. All real worker
 * selection, publishing, stealing and joining code is compiled below. */
void pxa_raster_compute_execute_range(const pxa_raster_compute_t *batch,
                                     uint32_t begin, uint32_t end) {
    if (current_task) {
        assert(calling_core == expected_core);
        atomic_fetch_add(&executions[calling_core], 1);
    }
    for (uint32_t i = begin; i < end; ++i) {
        assert(batch->target[i] == 0);
        batch->target[i] = batch->records[0];
        if (!(i & 127u)) sched_yield();
    }
}
void pxa_raster_execute_draw_list_rows(const uint8_t *bytes,
    const pxa_raster_draw_list_view_t *list, const pxa_raster_target_t *target,
    const pxa_raster_resources_t *resources, uint16_t begin, uint16_t end,
    pxa_raster_telemetry_t *telemetry) {
    (void)list; (void)resources; (void)telemetry;
    if (current_task) {
        assert(calling_core == expected_core);
        atomic_fetch_add(&executions[calling_core], 1);
    }
    for (unsigned i = begin * target->width; i < end * target->width; ++i) {
        assert(target->pixels[i] == 0);
        target->pixels[i] = bytes[0];
        if (!(i & 63u)) sched_yield();
    }
}

#include PXA_RASTER_WORKER_IMPLEMENTATION

int main(void) {
    unsigned stolen = 0, joined = 0;
    assert(current_raster_split(100) == 80);
    update_raster_split(100, 80, 80, 20);
    for (unsigned generation = 1; generation <= 4000; ++generation) {
        calling_core = generation & 1u;
        expected_core = calling_core ^ 1u;
        pxa_esp_raster_worker_t *worker = ensure_raster_worker();
        assert(worker && worker == &g_raster_workers[expected_core]);
        assert(worker->task->core != calling_core);
        /* Deliver stale/duplicate notifications to the worker that must not
         * touch this job, including immediately after switching callers. */
        if (g_raster_workers[calling_core].task)
            xTaskNotifyGive(g_raster_workers[calling_core].task);
        uint8_t input = (uint8_t)(generation % 251u + 1u);
        uint8_t buffer[1024] = {0};
        uint16_t pixels[32 * 32] = {0};
        pxa_raster_compute_t batch = {.records = &input, .target = buffer};
        memset(&g_raster_worker_job, 0, sizeof(g_raster_worker_job));
        if (generation & 2u) {
            g_raster_worker_job.compute_batch = &batch;
            g_raster_worker_job.byte_begin = 512;
            g_raster_worker_job.byte_end = 1024;
        } else {
            g_raster_worker_job.bytes = &input;
            g_raster_worker_job.target.pixels = pixels;
            g_raster_worker_job.target.width = 32;
            g_raster_worker_job.row_begin = 16;
            g_raster_worker_job.row_end = 32;
        }
        int steal = generation % 3u == 0;
        atomic_store(&worker->task->enabled, !steal);
        start_worker_job(worker);
        if (!steal) {
            while (__atomic_load_n(&worker->handoff.pending,
                                   __ATOMIC_ACQUIRE)) sched_yield();
        }
        if (generation & 2u)
            pxa_raster_compute_execute_range(&batch, 0, 512);
        else
            pxa_raster_execute_draw_list_rows(&input, &g_raster_worker_job.list,
                &g_raster_worker_job.target, &g_raster_worker_job.resources,
                0, 16, &g_raster_worker_job.telemetry);
        if (finish_worker_job(worker)) ++joined;
        else ++stolen;
        atomic_store(&worker->task->enabled, 1);
        for (unsigned i = 0; i < 1024; ++i)
            assert((generation & 2u ? buffer[i] : pixels[i]) == input);
        assert(!pxa_raster_handoff_claim(&worker->handoff));
    }
    assert(created == 2 && stolen && joined && stolen + joined == 4000);
    assert(atomic_load(&executions[0]) && atomic_load(&executions[1]));
    for (unsigned core = 0; core < 2; ++core) {
        pxa_esp_raster_worker_t *worker = &g_raster_workers[core];
        assert(pthread_cancel(worker->task->thread) == 0);
        assert(pthread_join(worker->task->thread, NULL) == 0);
        assert(sem_destroy(&worker->task->notification) == 0);
        assert(sem_destroy(worker->done) == 0);
    }
    printf("raster workers: 4000 jobs, %u stolen, %u joined, stale wakes safe\n",
           stolen, joined);
    return 0;
}
