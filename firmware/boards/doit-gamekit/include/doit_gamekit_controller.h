#pragma once
#include <cstddef>
#include <cstdint>

namespace doit_gamekit {
// Callers serialize these short operations. Posting to the Host happens outside
// the lock; Commit removes only the processed head (Guest or native UI), never a newer
// state installed by cancellation/takeover. Identical keepalives need no slot.
template<size_t Depth = 8>
class ControllerSamples {
public:
    static_assert(Depth >= 2 && Depth <= 255);
    struct Sample {
        uint32_t generation;
        uint16_t state;
        bool pending;
        bool active() const { return (state & kActive) != 0; }
        bool connected() const { return (state & kConnected) != 0; }
        uint32_t buttons() const { return state & kButtons; }
    };

    bool Push(bool connected, uint32_t buttons) {
        if (buttons > kButtons || (!connected && buttons)) return false;
        const uint16_t state = kActive | (connected ? kConnected : 0) | buttons;
        const uint16_t previous = count_
            ? queue_[(head_ + count_ - 1) % Depth] : current_;
        if (previous == state) return true;
        if (count_ == Depth) return false;
        queue_[(head_ + count_) % Depth] = state;
        ++count_;
        return true;
    }

    Sample Peek() const {
        return {generation_, count_ ? queue_[head_] : current_, count_ != 0};
    }

    bool Commit(const Sample& sample) {
        if (!sample.pending || !count_ || sample.generation != generation_ ||
            sample.state != queue_[head_]) return false;
        current_ = queue_[head_];
        head_ = (head_ + 1) % Depth;
        --count_;
        ++generation_;
        return true;
    }

    // Restore physical input and discard pending synthetic actions. A snapshot
    // already posted to the Host may complete; it cannot consume a new queue.
    void Clear() {
        head_ = count_ = 0;
        current_ = 0;
        ++generation_;
    }

private:
    static constexpr uint16_t kButtons = 4095, kActive = 1u << 12,
                              kConnected = 1u << 13;
    uint32_t generation_ = 0;
    uint16_t queue_[Depth]{}, current_ = 0;
    uint8_t head_ = 0, count_ = 0;
};
static_assert(sizeof(ControllerSamples<>) == 24);
}
