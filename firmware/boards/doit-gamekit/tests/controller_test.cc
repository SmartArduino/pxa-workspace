#include "doit_gamekit_controller.h"
#include <cassert>
#include <cstdint>
#include <deque>
#include <cstdio>

using doit_gamekit::ControllerSamples;

static void EdgeAndRetry() {
    ControllerSamples<> input;
    assert(!input.Peek().active());
    // Both edges arrive before the screen task polls; neither is overwritten.
    assert(input.Push(true, 1));
    assert(input.Push(true, 0));
    auto press = input.Peek();
    assert(press.pending && press.connected() && press.buttons() == 1);
    // A busy Host does not acknowledge the head. Heartbeats cannot replace it.
    for (int i = 0; i < 100; ++i) {
        assert(input.Push(true, 0));
        auto retry = input.Peek();
        assert(retry.generation == press.generation && retry.buttons() == 1);
    }
    assert(input.Commit(press));
    assert(!input.Commit(press));
    auto release = input.Peek();
    assert(release.pending && release.buttons() == 0);
    assert(input.Commit(release));
    assert(!input.Peek().pending && input.Peek().active());
    assert(input.Push(false, 0));
    auto disconnect = input.Peek();
    assert(disconnect.pending && !disconnect.connected());
    assert(input.Commit(disconnect));
    assert(!input.Peek().connected());
}

static void CapacityAndCancel() {
    ControllerSamples<2> input;
    assert(!input.Push(true, 4096));
    assert(!input.Push(false, 1));
    assert(!input.Peek().active());
    assert(input.Push(true, 1));
    auto old = input.Peek();
    assert(input.Push(true, 0));
    assert(!input.Push(true, 2)); // explicit rejection, no queued edge changed
    assert(input.Push(true, 0)); // identical keepalive succeeds even when full
    assert(input.Commit(old));
    assert(input.Push(true, 2));
    assert(input.Commit(input.Peek()));
    auto before_cancel = input.Peek();
    input.Clear();
    assert(!input.Peek().active() && !input.Peek().pending);
    assert(input.Push(true, 2)); // same value after cancel must still survive
    assert(!input.Commit(before_cancel));
    assert(input.Commit(input.Peek()));
    assert(input.Peek().buttons() == 2);
    input.Clear();
    assert(!input.Peek().active());
}

static void ReferenceSequence() {
    ControllerSamples<> input;
    std::deque<uint16_t> expected;
    uint16_t current = 0;
    uint32_t rng = 0x468391u;
    for (unsigned i = 0; i < 200000; ++i) {
        rng = rng * 1664525u + 1013904223u;
        const unsigned action = rng >> 28;
        if (action == 0) {
            input.Clear(); expected.clear(); current = 0;
        } else if (action < 10) {
            const bool connected = (rng & 16) != 0;
            const uint16_t buttons = connected ? (rng >> 8) & 4095 : 0;
            const uint16_t state = 4096 | (connected ? 8192 : 0) | buttons;
            const uint16_t last = expected.empty() ? current : expected.back();
            const bool accepted = last == state || expected.size() < 8;
            assert(input.Push(connected, buttons) == accepted);
            if (accepted && state != last) expected.push_back(state);
        } else if (action < 14) {
            auto sample = input.Peek();
            assert(input.Commit(sample) == !expected.empty());
            if (!expected.empty()) {
                current = expected.front(); expected.pop_front();
            }
        } // Remaining actions model a busy Host: Peek without Commit.
        auto sample = input.Peek();
        assert(sample.pending == !expected.empty());
        assert(sample.state == (expected.empty() ? current : expected.front()));
    }
}

int main() {
    EdgeAndRetry();
    CapacityAndCancel();
    ReferenceSequence();
    std::puts("controller edges, backpressure, takeover and 200000 model operations passed");
}
