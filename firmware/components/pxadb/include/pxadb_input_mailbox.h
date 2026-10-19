#pragma once

#include <cstddef>
#include <cstdint>

namespace pxadb {

enum class InputCommandKind : uint8_t {
    kPointer,
    kTap,
    kSwipe,
    kKey,
    kCancel,
    kSync,
    kController,
};

enum class PointerAction : uint8_t {
    kDown,
    kMove,
    kUp,
    kCancel,
};

enum class InputKey : uint8_t {
    kBack,
    kHome,
    kVolumeUp,
    kVolumeDown,
};

struct InputCommand {
    InputCommandKind kind = InputCommandKind::kSync;
    PointerAction pointer_action = PointerAction::kCancel;
    InputKey key = InputKey::kBack;
    uint32_t request_sequence = 0;
    uint32_t event_sequence = 0;
    uint16_t x = 0;
    uint16_t y = 0;
    uint16_t end_x = 0;
    uint16_t end_y = 0;
    uint16_t duration_ms = 0;
    uint8_t steps = 0;
    uint8_t pointer_id = 0;
    uint8_t controller = 0;
    bool connected = false;
    uint32_t buttons = 0;
};

inline bool CoordinateInBounds(uint16_t x, uint16_t y, uint16_t width,
                               uint16_t height) {
    return width != 0 && height != 0 && x < width && y < height;
}

inline uint16_t InterpolateCoordinate(uint16_t start, uint16_t end,
                                      uint16_t step, uint16_t steps) {
    if (steps == 0 || step >= steps) return end;
    const int32_t delta = static_cast<int32_t>(end) - start;
    return static_cast<uint16_t>(static_cast<int32_t>(start) +
                                 delta * step / steps);
}

enum class MailboxPushResult : uint8_t {
    kQueued,
    kCoalesced,
    kFull,
};

template <size_t Capacity>
class InputMailbox {
public:
    static_assert(Capacity >= 3, "input mailbox must preserve edge events");

    MailboxPushResult Push(const InputCommand& command,
                           InputCommand* replaced = nullptr) {
        if (command.kind == InputCommandKind::kPointer &&
            command.pointer_action == PointerAction::kMove && count_ != 0) {
            // Interleaved fingers retain one latest MOVE each. Never rewrite
            // an event across a reliable edge or an explicit SYNC barrier.
            for (size_t offset = count_; offset != 0; --offset) {
                InputCommand& pending = commands_[(head_ + offset - 1) % Capacity];
                if (pending.kind != InputCommandKind::kPointer ||
                    pending.pointer_action != PointerAction::kMove) break;
                if (pending.pointer_id == command.pointer_id) {
                    if (replaced != nullptr) *replaced = pending;
                    pending = command;
                    return MailboxPushResult::kCoalesced;
                }
            }
        }
        if (count_ == Capacity) {
            // Edge commands may evict an old MOVE, but never DOWN, UP, CANCEL,
            // TAP, SWIPE, KEY or SYNC.
            const bool pointer_edge =
                command.kind == InputCommandKind::kPointer &&
                command.pointer_action != PointerAction::kMove;
            if (!pointer_edge && command.kind != InputCommandKind::kCancel) {
                return MailboxPushResult::kFull;
            }
            size_t move_offset = count_;
            for (size_t offset = 0; offset < count_; ++offset) {
                const InputCommand& candidate =
                    commands_[(head_ + offset) % Capacity];
                if (candidate.kind == InputCommandKind::kPointer &&
                    candidate.pointer_action == PointerAction::kMove) {
                    move_offset = offset;
                    break;
                }
            }
            if (move_offset == count_) return MailboxPushResult::kFull;
            if (replaced != nullptr)
                *replaced = commands_[(head_ + move_offset) % Capacity];
            for (size_t offset = move_offset; offset + 1 < count_; ++offset) {
                commands_[(head_ + offset) % Capacity] =
                    commands_[(head_ + offset + 1) % Capacity];
            }
            --count_;
        }
        commands_[(head_ + count_) % Capacity] = command;
        ++count_;
        return MailboxPushResult::kQueued;
    }

    bool Take(InputCommand* command) {
        if (command == nullptr || count_ == 0) return false;
        *command = commands_[head_];
        head_ = (head_ + 1) % Capacity;
        --count_;
        return true;
    }

    void Clear() {
        head_ = 0;
        count_ = 0;
    }

    size_t size() const { return count_; }

private:
    InputCommand commands_[Capacity] = {};
    size_t head_ = 0;
    size_t count_ = 0;
};

class PointerState {
public:
    static constexpr uint8_t kMaximumPointers = 16;
    bool Apply(PointerAction action, uint8_t pointer_id) {
        if (pointer_id >= kMaximumPointers) return false;
        const uint16_t bit = uint16_t{1} << pointer_id;
        switch (action) {
            case PointerAction::kDown:
                if (mask_ & bit) return false;
                mask_ |= bit;
                return true;
            case PointerAction::kMove:
                return (mask_ & bit) != 0;
            case PointerAction::kUp:
            case PointerAction::kCancel:
                if (!(mask_ & bit)) return false;
                mask_ &= ~bit;
                return true;
        }
        return false;
    }

    bool pressed() const { return mask_ != 0; }
    uint8_t pointer_id() const {
        for (uint8_t id = 0; id < kMaximumPointers; ++id)
            if (mask_ & (uint16_t{1} << id)) return id;
        return 0;
    }
    void Reset() { mask_ = 0; }

private:
    uint16_t mask_ = 0;
};

// Commit ownership only after the adapter accepts the sample. A failed DOWN
// may already have changed its device state: cancel it even though it never
// became a logically delivered pointer. This also keeps wake-only gestures
// isolated from the other fingers that were actually delivered.
template<class Adapter>
bool DeliverPointer(PointerState& state, uint16_t& delivered, Adapter& adapter,
                    PointerAction action, uint16_t x, uint16_t y, uint8_t id) {
    if (id >= adapter.max_pointers) return false;
    PointerState next = state;
    if (!next.Apply(action, id)) return false;
    const uint16_t bit = uint16_t{1} << id;
    bool success = true;
    if (action == PointerAction::kDown) {
        const bool routed = adapter.route_pointer_down(adapter.context);
        success = !routed || adapter.inject_pointer(adapter.context, id, x, y, true);
        if (success && routed) delivered |= bit;
        if (!success && routed) (void)adapter.cancel_pointer(adapter.context, id);
    } else if (delivered & bit) {
        success = action == PointerAction::kCancel
            ? adapter.cancel_pointer(adapter.context, id)
            : adapter.inject_pointer(adapter.context, id, x, y,
                                     action == PointerAction::kMove);
    }
    if (success) {
        state = next;
        if (action == PointerAction::kUp || action == PointerAction::kCancel)
            delivered &= ~bit;
    }
    return success;
}

}  // namespace pxadb
