#include "include/pxadb_input_mailbox.h"
#include "include/pxadb_pointer_samples.h"

#include <cassert>

using namespace pxadb;

struct Adapter {
    uint8_t max_pointers = 2;
    void* context = this;
    bool hardware[2] = {};
    bool routed = true, fail_down = false;
    unsigned samples = 0, cancelled = 0;
    bool (*route_pointer_down)(void*) = [](void* ctx) {
        return static_cast<Adapter*>(ctx)->routed;
    };
    bool (*inject_pointer)(void*, uint8_t, uint16_t, uint16_t, bool) =
        [](void* ctx, uint8_t id, uint16_t, uint16_t, bool pressed) {
            auto& self = *static_cast<Adapter*>(ctx);
            self.hardware[id] = pressed;
            ++self.samples;
            return !self.fail_down;
        };
    bool (*cancel_pointer)(void*, uint8_t) = [](void* ctx, uint8_t id) {
        auto& self = *static_cast<Adapter*>(ctx);
        self.hardware[id] = false;
        ++self.cancelled;
        return true;
    };
};

int main() {
    {
    PointerSamples<5, 3> samples;
    bool more = false;
    assert(!samples.active() && !samples.Push(5, 1, 2, true));
    for (uint8_t id = 0; id < 5; ++id) assert(samples.Push(id, id*10, 2, true));
    assert(samples.Push(0, 11, 12, true));
    assert(samples.Push(0, 13, 14, true)); // Only MOVE is coalesced.
    assert(samples.Push(0, 15, 16, false));
    assert(!samples.Push(0, 17, 18, true)); // Full edges are never overwritten.
    auto sample = samples.Read(0, more);
    assert(sample.pressed && sample.x == 0 && more);
    sample = samples.Read(0, more);assert(sample.pressed && sample.x == 13 && more);
    sample = samples.Read(0, more);assert(!sample.pressed && sample.x == 15 && !more);
    for (uint8_t id = 1; id < 5; ++id) {
        sample = samples.Read(id, more);assert(sample.pressed && sample.x == id*10);
    }
    assert(samples.Cancel(2));sample = samples.Read(2, more);assert(sample.cancel && !sample.pressed);
    sample = samples.Read(1, more);assert(sample.pressed); // One release cannot release another finger.
    for (uint8_t id = 0; id < 5; ++id) {
        assert(samples.Cancel(id));sample = samples.Read(id, more);assert(!sample.pressed);
    }
    assert(!samples.active());
    for (uint8_t id = 0; id < 5; ++id) assert(samples.Push(id, 1, 2, true));
    samples.Clear();assert(!samples.active());
    }
    static_assert(sizeof(PointerState) == 2); // Same storage as the old single pointer.
    PointerState pointers;
    assert(!pointers.Apply(PointerAction::kMove, 0));
    assert(!pointers.Apply(PointerAction::kDown, 16));
    for (uint8_t id = 0; id < 16; ++id) {
        assert(pointers.Apply(PointerAction::kDown, id));
        assert(!pointers.Apply(PointerAction::kDown, id));
    }
    for (uint8_t id = 0; id < 16; ++id) {
        assert(pointers.pointer_id() == id);
        assert(pointers.Apply(PointerAction::kMove, id));
        assert(pointers.Apply(PointerAction::kCancel, id));
        assert(!pointers.Apply(PointerAction::kUp, id));
    }
    assert(!pointers.pressed());
    assert(pointers.Apply(PointerAction::kDown, 1));
    assert(pointers.Apply(PointerAction::kDown, 0));
    assert(pointers.Apply(PointerAction::kUp, 0) && pointers.pressed());
    assert(pointers.Apply(PointerAction::kMove, 1));
    pointers.Reset();
    assert(!pointers.pressed() && !pointers.Apply(PointerAction::kMove, 1));

    Adapter adapter;
    uint16_t delivered = 0;
    // A timed-out DOWN changed hardware, but must leave no stuck logical or
    // physical finger. This regression requires a partially failing adapter.
    adapter.fail_down = true;
    assert(!DeliverPointer(pointers, delivered, adapter, PointerAction::kDown, 1, 2, 0));
    assert(!pointers.pressed() && !delivered && !adapter.hardware[0]);
    assert(adapter.cancelled == 1);
    adapter.fail_down = false;
    assert(DeliverPointer(pointers, delivered, adapter, PointerAction::kDown, 1, 2, 0));
    assert(DeliverPointer(pointers, delivered, adapter, PointerAction::kDown, 3, 4, 1));
    assert(delivered == 3 && adapter.hardware[0] && adapter.hardware[1]);
    assert(DeliverPointer(pointers, delivered, adapter, PointerAction::kCancel, 0, 0, 1));
    assert(delivered == 1 && adapter.hardware[0] && !adapter.hardware[1]);
    assert(DeliverPointer(pointers, delivered, adapter, PointerAction::kUp, 1, 2, 0));
    assert(!pointers.pressed() && !delivered && !adapter.hardware[0]);
    // Swallowing a wake-only finger must not swallow another delivered finger.
    adapter.routed = false;
    unsigned samples = adapter.samples;
    assert(DeliverPointer(pointers, delivered, adapter, PointerAction::kDown, 1, 2, 0));
    adapter.routed = true;
    assert(DeliverPointer(pointers, delivered, adapter, PointerAction::kDown, 3, 4, 1));
    assert(DeliverPointer(pointers, delivered, adapter, PointerAction::kMove, 1, 2, 0));
    assert(DeliverPointer(pointers, delivered, adapter, PointerAction::kUp, 1, 2, 0));
    assert(adapter.samples == samples + 1 && delivered == 2 && pointers.pressed());
    assert(DeliverPointer(pointers, delivered, adapter, PointerAction::kCancel, 0, 0, 1));
    assert(!delivered && !pointers.pressed());

    InputMailbox<4> mailbox;
    InputCommand event;
    event.kind = InputCommandKind::kPointer;
    event.pointer_action = PointerAction::kMove;
    // Repeated dual-finger motion must not turn a tiny queue into old history.
    for (unsigned sample = 0; sample < 100; ++sample) {
        event.pointer_id = sample % 2;
        event.x = sample;
        const auto status = mailbox.Push(event);
        assert(status == (sample < 2 ? MailboxPushResult::kQueued
                                    : MailboxPushResult::kCoalesced));
    }
    assert(mailbox.size() == 2);
    assert(mailbox.Take(&event) && event.pointer_id == 0 && event.x == 98);
    assert(mailbox.Take(&event) && event.pointer_id == 1 && event.x == 99);
    assert(!mailbox.Take(&event));
    // A barrier preserves old coordinates and cannot be crossed by coalescing.
    event.pointer_id = 0; event.x = 10;
    assert(mailbox.Push(event) == MailboxPushResult::kQueued);
    InputCommand barrier;
    barrier.kind = InputCommandKind::kSync;
    assert(mailbox.Push(barrier) == MailboxPushResult::kQueued);
    event.x = 20;
    assert(mailbox.Push(event) == MailboxPushResult::kQueued);
    event.pointer_action = PointerAction::kUp;
    assert(mailbox.Push(event) == MailboxPushResult::kQueued);
    event.pointer_action = PointerAction::kMove;
    event.x = 30;
    assert(mailbox.Push(event) == MailboxPushResult::kFull);
    assert(mailbox.Take(&event) && event.x == 10);
    assert(mailbox.Take(&event) && event.kind == InputCommandKind::kSync);
    assert(mailbox.Take(&event) && event.x == 20);
    assert(mailbox.Take(&event) && event.pointer_action == PointerAction::kUp);
    // Across a wrapped queue, a reliable release may evict MOVE, never DOWN.
    event.pointer_action = PointerAction::kDown;
    assert(mailbox.Push(event) == MailboxPushResult::kQueued);
    event.pointer_action = PointerAction::kMove;
    assert(mailbox.Push(event) == MailboxPushResult::kQueued);
    assert(mailbox.Push(barrier) == MailboxPushResult::kQueued);
    event.pointer_id = 1;
    assert(mailbox.Push(event) == MailboxPushResult::kQueued);
    event.pointer_action = PointerAction::kCancel;
    assert(mailbox.Push(event) == MailboxPushResult::kQueued);
    assert(mailbox.Take(&event) && event.pointer_action == PointerAction::kDown);
    assert(mailbox.Take(&event) && event.kind == InputCommandKind::kSync);
    assert(mailbox.Take(&event) && event.pointer_id == 1 &&
           event.pointer_action == PointerAction::kMove);
    assert(mailbox.Take(&event) && event.pointer_action == PointerAction::kCancel);
    // Complete controller states carry reliable press/release edges. They
    // must neither coalesce with pointer motion nor cross a SYNC barrier.
    event.kind = InputCommandKind::kController;
    event.connected = true; event.buttons = 16;
    assert(mailbox.Push(event) == MailboxPushResult::kQueued);
    assert(mailbox.Push(barrier) == MailboxPushResult::kQueued);
    event.buttons = 0;
    assert(mailbox.Push(event) == MailboxPushResult::kQueued);
    assert(mailbox.Take(&event) && event.buttons == 16 && event.connected);
    assert(mailbox.Take(&event) && event.kind == InputCommandKind::kSync);
    assert(mailbox.Take(&event) && event.kind == InputCommandKind::kController && !event.buttons);
}
