// SPDX-License-Identifier: Apache-2.0
#include <pxa/pxa_worker_notifications.h>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

static std::atomic<bool> hold{true}, deleted{false};
static std::atomic<unsigned> entered{0}, calls{0}, waits{0};
template<typename F> static void Wait(F condition) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!condition()) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
}
void xTaskNotifyGive(TaskHandle_t task) {
    assert(task == reinterpret_cast<void*>(1) && !deleted);
    ++entered;
    Wait([]{return !hold;});
    assert(!deleted); // A worker cannot delete its task before this call returns.
    ++calls;
}
void vTaskDelay(TickType_t) { ++waits; std::this_thread::yield(); }
int main() {
    PxaWorkerNotifications gate;
    std::vector<std::thread> producers;
    for (unsigned i=0; i<4; ++i)
        producers.emplace_back([&]{assert(gate.Notify(reinterpret_cast<void*>(1)));});
    Wait([]{return entered==4;});
    std::thread worker([&]{gate.Stop(); deleted=true;});
    Wait([]{return waits.load()!=0;});
    for (unsigned i=0; i<100; ++i) assert(!gate.Notify(reinterpret_cast<void*>(1)));
    assert(!deleted && !calls && entered==4);
    hold=false;
    for (auto& thread:producers) thread.join();
    worker.join();
    assert(deleted && calls==4);
    for (unsigned i=0; i<100; ++i) assert(!gate.Notify(reinterpret_cast<void*>(1)));
    assert(!gate.Notify(nullptr)); gate.Stop();
    puts("Worker notifications: concurrent producers drained before task deletion; late wakeups rejected");
}
