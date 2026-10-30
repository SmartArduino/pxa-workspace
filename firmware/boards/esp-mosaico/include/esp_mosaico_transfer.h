#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "esp_mosaico_config.h"

namespace mosaico_board {

// Collect one refresh without expanding small dirty areas to full rows. The
// persistent frame preserves the gaps; the transfer planner chooses a window
// that is safe for the panel's scan rate after a fresh TE edge.
class RefreshRegion {
public:
    template <typename Area>
    bool Merge(const Area& area, const uint8_t* source, uint32_t stride,
               uint8_t* frame) {
        if (!ValidDisplayArea(area) || !source || !frame ||
            stride < (area.x2 - area.x1 + 1u) * sizeof(uint16_t)) return false;
        const size_t bytes = (area.x2 - area.x1 + 1) * sizeof(uint16_t);
        if (bytes == kWidth * sizeof(uint16_t) && stride == bytes) {
            std::memcpy(frame + area.y1 * bytes, source,
                        (area.y2 - area.y1 + 1) * bytes);
        } else {
            for (int32_t y = area.y1; y <= area.y2; ++y)
                std::memcpy(frame + (y * kWidth + area.x1) * sizeof(uint16_t),
                            source + (y - area.y1) * stride, bytes);
        }
        return Include(area);
    }

    template <typename Area>
    bool Include(const Area& area) {
        if (!ValidDisplayArea(area)) return false;
        const bool pending = last_ >= 0;
        left_ = pending ? std::min<int32_t>(left_, area.x1) : area.x1;
        right_ = pending ? std::max<int32_t>(right_, area.x2) : area.x2;
        first_ = pending ? std::min<int32_t>(first_, area.y1) : area.y1;
        last_ = pending ? std::max<int32_t>(last_, area.y2) : area.y2;
        return true;
    }

    template <typename Area>
    bool Take(Area* area) {
        if (last_ < 0 || !area) return false;
        *area = {left_, first_, right_, last_};
        last_ = -1;
        return true;
    }

private:
    int16_t left_ = 0;
    int16_t right_ = 0;
    int16_t first_ = 0;
    int16_t last_ = -1;
};

// Wide scroll regions use full rows for contiguous copies or one PPA transfer.
// Narrow updates copy only dirty pixels, preserving the surrounding snapshot.
template <typename Area>
Area SnapshotArea(Area area) {
    if (area.x2 - area.x1 + 1 >= kWidth * 3 / 4) {
        area.x1 = 0;
        area.x2 = kWidth - 1;
    }
    return area;
}

// Both persistent frames contain the previous complete image outside dirty
// pixels. Copying extra pixels is safe, including when TE later widens the
// panel window to full rows. The caller owns the entire two frames.
template <typename Area>
bool CopyRefreshArea(const Area& dirty, const uint8_t* frame, uint8_t* snapshot,
                     bool swap_bytes = false) {
    if (!ValidDisplayArea(dirty) || !frame || !snapshot) return false;
    const Area area = SnapshotArea(dirty);
    const size_t row_bytes = kWidth * sizeof(uint16_t);
    const size_t bytes = (area.x2 - area.x1 + 1) * sizeof(uint16_t);
    if (swap_bytes) {
        // A paired conversion is also the fallback when PPA is unavailable.
        // memcpy keeps unaligned buffers and strict aliasing well-defined.
        for (int32_t y = area.y1; y <= area.y2; ++y) {
            const size_t offset = y * row_bytes + area.x1 * sizeof(uint16_t);
            size_t x = 0;
            for (; x + 4 <= bytes; x += 4) {
                uint32_t pair;
                std::memcpy(&pair, frame + offset + x, 4);
                pair = ((pair & 0x00ff00ffu) << 8) | ((pair & 0xff00ff00u) >> 8);
                std::memcpy(snapshot + offset + x, &pair, 4);
            }
            if (x < bytes) {
                uint16_t pixel;
                std::memcpy(&pixel, frame + offset + x, 2);
                pixel = __builtin_bswap16(pixel);
                std::memcpy(snapshot + offset + x, &pixel, 2);
            }
        }
        return true;
    }
    if (bytes == row_bytes) {
        const size_t offset = area.y1 * row_bytes;
        std::memcpy(snapshot + offset, frame + offset, (area.y2 - area.y1 + 1) * bytes);
    } else {
        for (int32_t y = area.y1; y <= area.y2; ++y) {
            const size_t offset = y * row_bytes + area.x1 * sizeof(uint16_t);
            std::memcpy(snapshot + offset, frame + offset, bytes);
        }
    }
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

enum class FrameDamage { Unavailable, Unchanged, Changed };

// Compare two complete, immutable panel-order snapshots. The caller fences DMA
// before reading the previous one. A diagnostic overlay can be excluded because
// the flush worker redraws it in the new snapshot and merges its own damage.
template <typename Area>
FrameDamage FindFrameDamage(const uint8_t* previous, const uint8_t* next,
                           Area* area, const Area* excluded = nullptr) {
    if (!previous || !next || !area || (excluded && !ValidDisplayArea(*excluded)))
        return FrameDamage::Unavailable;
    int left = kWidth, right = -1, top = kHeight, bottom = -1;
    for (int y = 0; y < kHeight; ++y) {
        const bool exclude_row = excluded && y >= excluded->y1 && y <= excluded->y2;
        const size_t row = size_t(y) * kWidth * sizeof(uint16_t);
        for (int x = 0; x < kWidth; x += 2) {
            uint32_t before, after;
            std::memcpy(&before, previous + row + x * sizeof(uint16_t), 4);
            std::memcpy(&after, next + row + x * sizeof(uint16_t), 4);
            uint32_t difference = before ^ after;
            if (!difference) continue;
            if (exclude_row) {
                if (x >= excluded->x1 && x <= excluded->x2) difference &= 0xffff0000u;
                if (x + 1 >= excluded->x1 && x + 1 <= excluded->x2) difference &= 0x0000ffffu;
                if (!difference) continue;
            }
            left = std::min(left, x + ((difference & 0xffffu) ? 0 : 1));
            right = std::max(right, x + ((difference >> 16) ? 1 : 0));
            top = std::min(top, y);
            bottom = y;
        }
    }
    if (right < 0) {
        // Retain normal frame completion/TE ownership without retransmitting
        // a visually identical full frame. This is one minimal aligned window.
        *area = {0, 0, 3, 1};
        return FrameDamage::Unchanged;
    }
    *area = {static_cast<decltype(area->x1)>(left), static_cast<decltype(area->y1)>(top),
             static_cast<decltype(area->x2)>(right), static_cast<decltype(area->y2)>(bottom)};
    RoundDisplayArea(area);
    return FrameDamage::Changed;
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

template <typename Area>
struct TransferPlan {
    Area area;
    uint32_t start_us;
    uint32_t deadline_us;
    uint32_t budget_us;
    bool compact;
};

// A narrow, fast writer can overtake the scanning panel. Use it only when the
// whole window fits between scans: preferably before the first row, otherwise
// after the last row. Long windows retain the full-row scan-follow path.
// No TE or an implausible measurement also uses that conservative path.
template <typename Area>
TransferPlan<Area> PlanTransfer(Area dirty, uint32_t period_us, uint32_t blank_us,
                                bool compact_enabled = true) {
    RoundDisplayArea(&dirty);
    const bool known = period_us >= 12000 && period_us <= 25000 && blank_us < period_us / 3;
    const uint32_t period = known ? period_us : 16667;
    const uint32_t active = known ? period - blank_us : period;
    const uint32_t guard = 250;
    const uint32_t start = (static_cast<uint64_t>(dirty.y2 + 1) * active + kHeight - 1) /
                           kHeight + guard;
    const uint32_t deadline = period + static_cast<uint64_t>(dirty.y1) * active / kHeight - guard;
    const uint32_t height = dirty.y2 - dirty.y1 + 1;
    const uint32_t width = dirty.x2 - dirty.x1 + 1;
    const uint32_t wire_us = (static_cast<uint64_t>(width) * height * 16 * 1000000 +
                              4 * kPixelClockHz - 1) / (4 * kPixelClockHz);
    // Half wire rate plus per-band command/copy overhead and scheduling margin.
    const uint32_t budget = wire_us * 2 + ((height + kTransferRows - 1) / kTransferRows) * 80 + 2000;
    if (known && compact_enabled && width < kWidth) {
        const uint32_t first_scan = static_cast<uint64_t>(dirty.y1) * active / kHeight;
        if (first_scan >= 2 * guard + budget + 1000)
            return {dirty, guard, first_scan - guard, budget, true};
        if (deadline > start && deadline - start >= budget + 1000)
            return {dirty, start, deadline, budget, true};
    }
    dirty.x1 = 0;
    dirty.x2 = kWidth - 1;
    return {dirty, ScanFollowDelayUs(dirty.y1, active), static_cast<uint32_t>(
            period + static_cast<uint64_t>(dirty.y2) * active / kHeight - guard), 0, false};
}

enum class DirectTransferResult { Ineligible, Completed, Failed };

// The LCD IO driver splits a contiguous span at the SPI transaction limit and
// signals completion only for its final chunk. No internal bounce copy is needed.
// A failed wait/submit must not fall back: DMA can still own the source span.
template <typename Area, typename Wait, typename Submit>
DirectTransferResult TransferContiguousRows(const Area& area, const uint8_t* pixels,
                                            uint32_t stride, Wait wait, Submit submit) {
    if (!ValidDisplayArea(area) || !pixels || area.x1 != 0 || area.x2 != kWidth - 1 ||
        stride != kWidth * sizeof(uint16_t) ||
        reinterpret_cast<uintptr_t>(pixels) % 64 != 0)
        return DirectTransferResult::Ineligible;
    if (!wait() || !submit(pixels, size_t(area.y2 - area.y1 + 1) * stride))
        return DirectTransferResult::Failed;
    return wait() ? DirectTransferResult::Completed : DirectTransferResult::Failed;
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
        const uint8_t* source = pixels + (y - area.y1) * stride;
        if (stride == row_bytes) {
            std::memcpy(buffers[index], source, rows * row_bytes);
        } else {
            for (int32_t row = 0; row < rows; ++row)
                std::memcpy(buffers[index] + row * row_bytes,
                            source + row * stride, row_bytes);
        }
        if (!wait() || !submit(y, rows, buffers[index])) return false;
        mirror(y, rows);
        index ^= 1;
    }
    return wait();
}

}  // namespace mosaico_board
