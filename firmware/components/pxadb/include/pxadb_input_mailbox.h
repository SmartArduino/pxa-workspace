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
            const size_t tail = (head_ + count_ - 1) % Capacity;
            InputCommand& pending = commands_[tail];
            if (pending.kind == InputCommandKind::kPointer &&
                pending.pointer_action == PointerAction::kMove &&
                pending.pointer_id == command.pointer_id) {
                if (replaced != nullptr) *replaced = pending;
                pending = command;
                return MailboxPushResult::kCoalesced;
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
    bool Apply(PointerAction action, uint8_t pointer_id) {
        if (pointer_id != 0) return false;
        switch (action) {
            case PointerAction::kDown:
                if (pressed_) return false;
                pressed_ = true;
                pointer_id_ = pointer_id;
                return true;
            case PointerAction::kMove:
                return pressed_ && pointer_id_ == pointer_id;
            case PointerAction::kUp:
                if (!pressed_ || pointer_id_ != pointer_id) return false;
                pressed_ = false;
                return true;
            case PointerAction::kCancel:
                if (!pressed_ || pointer_id_ != pointer_id) return false;
                pressed_ = false;
                return true;
        }
        return false;
    }

    bool pressed() const { return pressed_; }
    uint8_t pointer_id() const { return pointer_id_; }
    void Reset() {
        pressed_ = false;
        pointer_id_ = 0;
    }

private:
    bool pressed_ = false;
    uint8_t pointer_id_ = 0;
};

}  // namespace pxadb
