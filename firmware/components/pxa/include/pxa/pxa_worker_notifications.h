// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <atomic>
#include <cstdint>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Board-lifetime gate for a worker that deletes itself. Initialize the task
// handle before publishing it to callers. Only the worker calls Stop(), after
// draining its device I/O and immediately before vTaskDelete(nullptr).
class PxaWorkerNotifications {
public:
    bool Notify(TaskHandle_t task) {
        in_flight_.fetch_add(1, std::memory_order_seq_cst);
        const bool accepted = task && !stopped_.load(std::memory_order_seq_cst);
        if (accepted) xTaskNotifyGive(task);
        in_flight_.fetch_sub(1, std::memory_order_seq_cst);
        return accepted;
    }

    void Stop() {
        // Sequential consistency across both atomics is required: a notifier
        // either sees stopped, or its reference must be visible to this drain.
        stopped_.store(true, std::memory_order_seq_cst);
        while (in_flight_.load(std::memory_order_seq_cst)) vTaskDelay(1);
    }

private:
    std::atomic<uint32_t> in_flight_{0};
    std::atomic<bool> stopped_{false};
};
