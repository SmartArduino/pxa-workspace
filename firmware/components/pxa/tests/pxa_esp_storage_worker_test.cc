// SPDX-License-Identifier: Apache-2.0
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
#include "../src/services/pxa_esp_storage_worker.c"

struct Queue {
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::vector<unsigned char>> items;
    unsigned capacity; size_t size; bool closing = false;
};
struct Semaphore { std::mutex mutex; std::condition_variable changed; bool ready = false; };
struct StopWorker {};
static std::thread *worker_thread;
QueueHandle_t xQueueCreate(unsigned capacity, size_t size) {
    auto *q = new Queue; q->capacity = capacity; q->size = size; return q;
}
int xQueueSend(QueueHandle_t handle, const void *item, uint32_t timeout) {
    auto *q = static_cast<Queue *>(handle); std::unique_lock lock(q->mutex);
    if (!timeout && q->items.size() == q->capacity) return 0;
    q->changed.wait(lock, [&]{ return q->items.size() < q->capacity; });
    q->items.emplace_back(q->size); std::memcpy(q->items.back().data(), item, q->size);
    q->changed.notify_all(); return pdTRUE;
}
int xQueueReceive(QueueHandle_t handle, void *item, uint32_t) {
    auto *q = static_cast<Queue *>(handle); std::unique_lock lock(q->mutex);
    q->changed.wait(lock, [&]{ return q->closing || !q->items.empty(); });
    if (q->closing) throw StopWorker{};
    std::memcpy(item, q->items.front().data(), q->size); q->items.pop_front();
    q->changed.notify_all(); return pdTRUE;
}
SemaphoreHandle_t xSemaphoreCreateBinary() { return new Semaphore; }
int xSemaphoreTake(SemaphoreHandle_t handle, uint32_t) {
    auto *s = static_cast<Semaphore *>(handle); std::unique_lock lock(s->mutex);
    s->changed.wait(lock, [&]{ return s->ready; }); s->ready = false; return pdTRUE;
}
void xSemaphoreGive(SemaphoreHandle_t handle) {
    auto *s = static_cast<Semaphore *>(handle); std::lock_guard lock(s->mutex);
    s->ready = true; s->changed.notify_one();
}
int xTaskCreate(void (*run)(void *), const char *, unsigned, void *context, unsigned, TaskHandle_t *handle) {
    worker_thread = new std::thread([=]{ try { run(context); } catch (StopWorker&) {} });
    *handle = worker_thread; return pdPASS;
}
int64_t esp_timer_get_time() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct Generation {
    std::mutex mutex; std::condition_variable changed;
    bool blocked = false, release = false, alive = true;
    std::atomic<unsigned> notifications{0};
    std::vector<unsigned> completed;
    std::thread::id owner = std::this_thread::get_id();
};
struct Job { Generation *generation; unsigned id; bool block; };
static void write_checkpoint(void *context) {
    auto &job = *static_cast<Job *>(context); auto &g = *job.generation;
    assert(std::this_thread::get_id() != g.owner);
    std::unique_lock lock(g.mutex); assert(g.alive);
    if (job.block) { g.blocked = true; g.changed.notify_one(); g.changed.wait(lock, [&]{ return g.release; }); }
    assert(g.alive); g.completed.push_back(job.id);
}
static void completion_ready(void *context) {
    auto &g = *static_cast<Generation *>(context);
    std::lock_guard lock(g.mutex); assert(g.alive); ++g.notifications;
}
int main() {
    assert(pxa_esp_storage_worker_begin(nullptr, nullptr) == PXA_STATUS_INVALID_ARGUMENT);
    Generation first; Job jobs[6];
    for (unsigned i = 0; i < 6; ++i) jobs[i] = {&first, i, i == 0};
    assert(pxa_esp_storage_worker_begin(completion_ready, &first) == PXA_STATUS_OK);
    assert(pxa_esp_storage_worker_submit(nullptr, write_checkpoint, &jobs[0]) == PXA_STATUS_OK);
    { std::unique_lock lock(first.mutex); first.changed.wait(lock, [&]{ return first.blocked; }); }
    for (unsigned i = 1; i < 5; ++i)
        assert(pxa_esp_storage_worker_submit(nullptr, write_checkpoint, &jobs[i]) == PXA_STATUS_OK);
    assert(pxa_esp_storage_worker_submit(nullptr, write_checkpoint, &jobs[5]) == PXA_STATUS_WOULD_BLOCK);
    // The runtime owner keeps processing turns while the first disk call is
    // explicitly held. Closing then waits for that call and every queued save.
    unsigned owner_turns = 0; for (; owner_turns < 100; ++owner_turns) assert(first.notifications == 0);
    std::thread release([&]{ std::this_thread::sleep_for(std::chrono::milliseconds(20));
        std::lock_guard lock(first.mutex); first.release = true; first.changed.notify_one(); });
    pxa_esp_storage_worker_end(); release.join();
    assert(owner_turns == 100 && first.notifications == 5);
    assert((first.completed == std::vector<unsigned>{0, 1, 2, 3, 4}));
    first.alive = false;
    assert(pxa_esp_storage_worker_submit(nullptr, write_checkpoint, &jobs[5]) == PXA_STATUS_BAD_STATE);
    for (unsigned i = 0; i < 64; ++i) {
        Generation next; Job job{&next, i, false};
        assert(pxa_esp_storage_worker_begin(completion_ready, &next) == PXA_STATUS_OK);
        assert(pxa_esp_storage_worker_submit(nullptr, write_checkpoint, &job) == PXA_STATUS_OK);
        pxa_esp_storage_worker_end();
        assert(next.notifications == 1 && next.completed == std::vector<unsigned>{i});
        next.alive = false;
    }
    auto *q = static_cast<Queue *>(g_queue);
    { std::lock_guard lock(q->mutex); q->closing = true; q->changed.notify_one(); }
    worker_thread->join(); delete worker_thread; delete q;
    delete static_cast<Semaphore *>(g_drained);
    puts("KV worker: blocked disk leaves owner runnable, FIFO/backpressure, close drains callbacks, and 64 activation handoffs have no late access");
}
