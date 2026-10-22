#pragma once
#include <cstdint>
#include <esp_log.h>
#include <esp_timer.h>
#include <pxa_board_api.h>

namespace pxa::board {
// UI-task-only timing. With performance logging off, no clocks are sampled.
class LcdRenderTrace {
public:
    enum Phase { Compose, Copy, Wait, Rotate, Count };
    void Begin() {
        active_ = pxa_board_performance_get(PXSYS_REFERENCE_PERFORMANCE_LOG);
        if (!active_) {
            window_ = 0;
            frames_ = tiles_ = surfaces_ = 0;
            total_ = 0;
            for (auto& sum : sums_) sum = 0;
            return;
        }
        started_ = esp_timer_get_time();
        if (!window_) window_ = started_;
    }
    uint64_t Mark() const { return active_ ? esp_timer_get_time() : 0; }
    void Add(Phase phase, uint64_t mark) {
        if (active_) sums_[phase] += esp_timer_get_time() - mark;
    }
    void Tile() { if (active_) ++tiles_; }
    void End(const char* tag, bool surface, bool direct) {
        if (!active_) return;
        const uint64_t now = esp_timer_get_time();
        total_ += now - started_; ++frames_; surfaces_ += surface;
        if (now - window_ < 5000000) return;
        uint64_t phases = 0; for (auto sum : sums_) phases += sum;
        const uint64_t drawing = total_ > phases ? total_ - phases : 0;
        ESP_LOGI(tag, "LCD UI mode=%s frames=%lu surfaces=%lu tiles_x100=%lu "
            "render_wall_avg_us=%lu draw_wall_avg_us=%lu compose_avg_us=%lu "
            "copy_avg_us=%lu wait_avg_us=%lu rotate_avg_us=%lu",
            direct ? "direct" : "partial", (unsigned long)frames_, (unsigned long)surfaces_,
            (unsigned long)(tiles_ * 100 / frames_), (unsigned long)(total_ / frames_),
            (unsigned long)(drawing / frames_), (unsigned long)(sums_[Compose] / frames_),
            (unsigned long)(sums_[Copy] / frames_), (unsigned long)(sums_[Wait] / frames_),
            (unsigned long)(sums_[Rotate] / frames_));
        window_ = 0; frames_ = tiles_ = surfaces_ = 0; total_ = 0;
        for (auto& sum : sums_) sum = 0;
    }
private:
    bool active_ = false;
    uint64_t started_ = 0, window_ = 0, total_ = 0, sums_[Count]{};
    uint32_t frames_ = 0, tiles_ = 0, surfaces_ = 0;
};
// Used only by the dedicated submit task. Draining the queue is included in
// the transfer duration; output buffers remain owned until this completes.
class LcdTransferTrace {
public:
    uint64_t Begin() const {
        return pxa_board_performance_get(PXSYS_REFERENCE_PERFORMANCE_LOG) ? esp_timer_get_time() : 0;
    }
    void End(const char* tag, uint64_t started, bool success) {
        if (!started) { window_ = sum_ = 0; count_ = errors_ = 0; return; }
        const uint64_t now = esp_timer_get_time();
        if (!window_) window_ = started;
        sum_ += now - started;
        ++count_;
        errors_ += !success;
        if (now - window_ < 5000000) return;
        ESP_LOGI(tag, "LCD SPI transfers=%lu errors=%lu transfer_wall_avg_us=%lu",
            (unsigned long)count_, (unsigned long)errors_, (unsigned long)(sum_ / count_));
        window_ = sum_ = 0;
        count_ = errors_ = 0;
    }
private:
    uint64_t window_ = 0, sum_ = 0;
    uint32_t count_ = 0, errors_ = 0;
};
}
