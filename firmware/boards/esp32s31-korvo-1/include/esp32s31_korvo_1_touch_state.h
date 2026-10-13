#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

enum class KorvoTouchReport { kNone, kReady, kError };

// GT1x exposes an acknowledged mailbox, not a live contact count. The generic
// touch driver discards the ready bit and acknowledges even an empty mailbox.
// Gate that driver before it can consume/clear a report that is not ready yet.
// Once ready, the controller holds the report until the driver acknowledges it.
template <class ReadStatus, class ReadPoints>
KorvoTouchReport ReadKorvoTouchReport(uint8_t& status,
                                    ReadStatus read_status, ReadPoints read_points) {
    if (!read_status(status)) return KorvoTouchReport::kError;
    if ((status & 0x80) == 0) return KorvoTouchReport::kNone;
    // The official driver acknowledges malformed counts as well. Consume the
    // mailbox, but never turn its invalid count into an apparent release.
    if (!read_points()) return KorvoTouchReport::kError;
    return (status & 0x0f) <= 10 ? KorvoTouchReport::kReady
                               : KorvoTouchReport::kError;
}

// Controller report order can change while fingers remain down. Each LVGL
// pointer keeps its contact ID until release, including a release interval
// before a full set of slots can be reused for different fingers.
template <size_t Capacity>
class KorvoTouchState {
public:
    static_assert(Capacity > 0);
    static constexpr int64_t kErrorReleaseTimeoutUs = 150000;

    struct Contact {
        uint16_t x = 0;
        uint16_t y = 0;
        uint8_t id = 0;
        bool pressed = false;
    };

    const Contact& operator[](size_t slot) const { return contacts_[slot]; }

    template <class Point>
    void Update(std::span<const Point> points, int64_t now_us,
                uint16_t width, uint16_t height) {
        bool seen[Capacity] = {};
        for (const auto& point : points) {
            size_t slot = Capacity;
            bool duplicate = false;
            for (size_t i = 0; i < Capacity; ++i) {
                if (contacts_[i].pressed && contacts_[i].id == point.track_id) {
                    // A duplicate ID must not create a second pointer.
                    if (seen[i]) { duplicate = true; break; }
                    slot = i;
                    break;
                }
            }
            if (duplicate) continue;
            if (point.x >= width || point.y >= height) {
                // A bad position must not fabricate an UP for an ID that the
                // controller still reports as down. Keep its last valid XY.
                if (slot != Capacity) seen[slot] = true;
                continue;
            }
            if (slot == Capacity) {
                for (size_t i = 0; i < Capacity; ++i) {
                    if (!contacts_[i].pressed && !seen[i]) {
                        slot = i;
                        break;
                    }
                }
            }
            if (slot == Capacity) continue;
            seen[slot] = true;
            contacts_[slot] = {point.x, point.y, point.track_id, true};
        }
        for (size_t i = 0; i < Capacity; ++i) {
            if (!seen[i]) contacts_[i].pressed = false;
        }
        last_successful_read_us_ = now_us;
    }

    // No fresh report means the contact state has not changed. It also proves
    // that the I2C link is healthy: stationary fingers must not time out.
    void NoReport(int64_t now_us) {
        last_successful_read_us_ = now_us;
    }

    void ReadFailed(int64_t now_us) {
        if (now_us < last_successful_read_us_ ||
            now_us - last_successful_read_us_ < kErrorReleaseTimeoutUs) return;
        for (auto& contact : contacts_) contact.pressed = false;
    }

private:
    Contact contacts_[Capacity] = {};
    int64_t last_successful_read_us_ = 0;
};
