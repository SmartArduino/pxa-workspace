#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "esp_mosaico_config.h"

namespace mosaico_board {

// Collect one LVGL refresh before sending it. The persistent frame preserves
// pixels between disjoint dirty rectangles; transfer complete rows so the
// 48 MHz writer stays slower than the panel's approximately 60 Hz scan.
class RefreshRows {
public:
    template <typename Area>
    bool Merge(const Area& area, const uint8_t* source, uint32_t stride,
               uint8_t* frame) {
        if (!ValidDisplayArea(area) || !source || !frame ||
            stride < (area.x2 - area.x1 + 1u) * sizeof(uint16_t)) return false;
        const size_t bytes = (area.x2 - area.x1 + 1) * sizeof(uint16_t);
        for (int32_t y = area.y1; y <= area.y2; ++y)
            std::memcpy(frame + (y * kWidth + area.x1) * sizeof(uint16_t),
                        source + (y - area.y1) * stride, bytes);
        first_ = pending_ ? std::min<int32_t>(first_, area.y1) : area.y1;
        last_ = pending_ ? std::max<int32_t>(last_, area.y2) : area.y2;
        pending_ = true;
        return true;
    }

    template <typename Area>
    bool Take(Area* area) {
        if (!pending_ || !area) return false;
        *area = {0, first_, kWidth - 1, last_};
        pending_ = false;
        return true;
    }

private:
    int32_t first_ = 0;
    int32_t last_ = 0;
    bool pending_ = false;
};

// Snapshot only the complete rows that will be sent. The producer can update
// its persistent frame immediately; the consumer owns snapshot until DMA ends.
template <typename Area>
bool CopyRefreshRows(const Area& area, const uint8_t* frame, uint8_t* snapshot) {
    if (!ValidDisplayArea(area) || area.x1 != 0 || area.x2 != kWidth - 1 ||
        !frame || !snapshot) return false;
    const size_t offset = area.y1 * kWidth * sizeof(uint16_t);
    const size_t bytes = (area.y2 - area.y1 + 1) * kWidth * sizeof(uint16_t);
    std::memcpy(snapshot + offset, frame + offset, bytes);
    return true;
}

// The caller has waited for the previous transfer. A complete frame can hand
// its storage to DMA; the producer's replacement must be restored before a
// later partial LVGL refresh, but a subsequent full render overwrites it.
template <typename Area>
bool ExchangeRefreshFrame(const Area& area, uint8_t*& frame, uint8_t*& snapshot) {
    if (!ValidDisplayArea(area) || area.x1 != 0 || area.y1 != 0 ||
        area.x2 != kWidth - 1 || area.y2 != kHeight - 1 ||
        !frame || !snapshot || frame == snapshot) return false;
    std::swap(frame, snapshot);
    return true;
}

inline void CaptureScanoutRgb565(const uint8_t* snapshot, uint16_t* pixels) {
    const auto* source = reinterpret_cast<const uint16_t*>(snapshot);
    for (size_t i = 0; i < kWidth * kHeight; ++i)
        pixels[i] = __builtin_bswap16(source[i]);
}

// TE falling marks the end of vertical blanking. Begin just behind the scan
// of the first affected row, avoiding the catch-up line of a slow writer that
// starts during blanking. The active interval excludes the measured TE pulse.
inline uint32_t ScanFollowDelayUs(int32_t first_row, uint32_t active_us) {
    return static_cast<uint32_t>(
        static_cast<uint64_t>(first_row + 1) * active_us / kHeight) + 250;
}

// At most one band is in flight. Copy into the other buffer while DMA reads
// the current one, then wait before submitting RAMWRC for the next band.
// A false wait/submit leaves the in-flight buffer untouched for recovery.
template <typename Area, typename Wait, typename Submit, typename Mirror>
bool TransferBands(const Area& area, const uint8_t* pixels, uint32_t stride,
                   uint8_t* const buffers[2], Wait wait, Submit submit,
                   Mirror mirror) {
    if (!wait()) return false;
    const size_t row_bytes = (area.x2 - area.x1 + 1) * sizeof(uint16_t);
    uint8_t index = 0;
    for (int32_t y = area.y1; y <= area.y2; y += kTransferRows) {
        const int32_t rows = std::min<int32_t>(kTransferRows, area.y2 - y + 1);
        for (int32_t row = 0; row < rows; ++row)
            std::memcpy(buffers[index] + row * row_bytes,
                        pixels + (y + row - area.y1) * stride, row_bytes);
        if (!wait() || !submit(y, rows, buffers[index])) return false;
        mirror(y, rows);
        index ^= 1;
    }
    return wait();
}

}  // namespace mosaico_board
