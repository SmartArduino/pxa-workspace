// SPDX-License-Identifier: MIT
#include <pxa/pxa_audio_capture.h>
#include <pxa/runtime.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>

using StaticSemaphore_t = std::mutex;
using SemaphoreHandle_t = std::mutex*;
static struct { std::mutex backend_lock; } g_audio;
#define portENTER_CRITICAL(mux) (mux)->lock()
#define portEXIT_CRITICAL(mux) (mux)->unlock()
constexpr uint32_t portMAX_DELAY = UINT32_MAX;
constexpr int pdTRUE = 1;
static SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t* lock) { return lock; }
static int xSemaphoreTake(SemaphoreHandle_t lock, uint32_t wait) {
    if (!wait) return lock->try_lock();
    lock->lock();
    return pdTRUE;
}
static void xSemaphoreGive(SemaphoreHandle_t lock) { lock->unlock(); }
#include "../src/services/pxa_esp_capture.inc"

static std::atomic<bool> hold_open{false}, opening{false};
static unsigned opens, closes;
static void WaitFor(const std::atomic<bool>& condition) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!condition) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
}
static pxa_status_t Open(void*) {
    ++opens;
    opening = true;
    while (hold_open) std::this_thread::yield();
    return PXA_STATUS_OK;
}
static int32_t Read(void*, uint8_t* output, size_t) {
    output[0] = 27;
    return 1;
}
static void Close(void*) { ++closes; }

int main() {
    uint64_t first = 0, second = 0;
    uint8_t pcm[PXA_AUDIO_CAPTURE_MAX_PACKET];
    pxa_esp_audio_set_capture_source(nullptr, 20, Open, Read, Close);
    assert(capture_frame_ms(nullptr) == 20);
    hold_open = true;
    std::thread start([&] { assert(capture_open(nullptr, &first) == PXA_STATUS_OK); });
    WaitFor(opening);
    assert(capture_io(nullptr, 1, PXA_IO_READ, pcm, sizeof(pcm)) == PXA_STATUS_WOULD_BLOCK);
    std::atomic<bool> suspending{false};
    std::thread suspend([&] { suspending = true; capture_set_suspended(true); });
    WaitFor(suspending);
    hold_open = false;
    start.join();
    suspend.join();
    assert(first && opens == 1 && closes == 1);
    assert(capture_io(nullptr, first, PXA_IO_READ, pcm, sizeof(pcm)) == PXA_STATUS_UNAVAILABLE);
    assert(capture_open(nullptr, &second) == PXA_STATUS_UNAVAILABLE && !second);
    capture_set_suspended(false);
    assert(capture_open(nullptr, &second) == PXA_STATUS_OK && second != first);
    capture_close(nullptr, first);
    assert(capture_io(nullptr, second, PXA_IO_READ, pcm, sizeof(pcm)) == 1 && pcm[0] == 27);
    capture_reset_sessions();
    capture_close(nullptr, second);
    assert(opens == 2 && closes == 2);
    puts("Capture registry: concurrent slow open/suspend, nonblocking read and stale close passed");
}
