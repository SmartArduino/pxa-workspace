#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <vector>

#include "esp_mosaico_transfer.h"

namespace {
struct Area { int32_t x1, y1, x2, y2; };
struct Case { const char* name; Area area; };

// This measures native CPU copies and models wire bytes. Neither is device FPS.
template <typename Copy>
double MedianCopyUs(Copy copy) {
    std::array<double, 9> trials;
    constexpr unsigned repeats = 512;
    for (auto& trial : trials) {
        const auto start = std::chrono::steady_clock::now();
        for (unsigned i = 0; i < repeats; ++i) {
            copy();
            // Keep each copy observable even under optimizing compilers.
            asm volatile("" ::: "memory");
        }
        trial = std::chrono::duration<double, std::micro>(
            std::chrono::steady_clock::now() - start).count() / repeats;
    }
    std::sort(trials.begin(), trials.end());
    return trials[trials.size() / 2];
}
}

int main() {
    constexpr size_t stride = mosaico_board::kWidth * sizeof(uint16_t);
    std::vector<uint8_t> frame(stride * mosaico_board::kHeight, 0x39);
    std::vector<uint8_t> scanout(frame.size());
    const Case cases[] = {
        {"top_indicator", {200, 0, 279, 39}},
        {"bottom_indicator", {200, 420, 279, 459}},
        {"toggle", {360, 220, 455, 279}},
        {"scroll", {12, 80, 467, 431}},
        {"fullscreen", {0, 0, 479, 479}}
    };
    std::puts("case,old_copy_bytes,new_copy_bytes,old_wire_bytes,new_wire_bytes,"
              "old_wire_min_us,new_wire_min_us,plan,old_host_copy_us,new_host_copy_us");
    for (const auto& item : cases) {
        const auto area = item.area;
        const size_t height = area.y2 - area.y1 + 1;
        const size_t old_bytes = stride * height;
        const auto copied = mosaico_board::SnapshotArea(area);
        const size_t new_bytes = (copied.x2 - copied.x1 + 1) * height * sizeof(uint16_t);
        const auto plan = mosaico_board::PlanTransfer(area, 16931, 800);
        const size_t wire_bytes = (plan.area.x2 - plan.area.x1 + 1) *
                                 (plan.area.y2 - plan.area.y1 + 1) * sizeof(uint16_t);
        const double old_us = MedianCopyUs([&]() {
            std::memcpy(scanout.data() + area.y1 * stride,
                        frame.data() + area.y1 * stride, old_bytes);
        });
        const double new_us = MedianCopyUs([&]() {
            mosaico_board::CopyRefreshArea(area, frame.data(), scanout.data());
        });
        std::printf("%s,%zu,%zu,%zu,%zu,%.3f,%.3f,%s,%.3f,%.3f\n",
            item.name, old_bytes, new_bytes, old_bytes, wire_bytes,
            old_bytes * 8.0 * 1000000 / (4.0 * mosaico_board::kPixelClockHz),
            wire_bytes * 8.0 * 1000000 / (4.0 * mosaico_board::kPixelClockHz),
            plan.compact ? "compact" : "full_rows", old_us, new_us);
    }
    std::fprintf(stderr, "model: 480x480 RGB565, QSPI 4x48 MHz, TE period=16931 us, blank=800 us; "
                 "CPU timings are host-only, exclude TE/DMA/rendering.\n");
}
