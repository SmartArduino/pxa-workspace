#include "esp32s31_korvo_1_touch_state.h"

#include <array>
#include <cassert>
#include <cstdio>

struct Point {
    uint16_t x, y;
    uint8_t track_id;
};

void TestReportMailbox() {
    // The consumed driver buffer is empty between reports. Polling it directly
    // would produce UP/DOWN pairs while a finger remains on the panel.
    KorvoTouchState<5> state;
    uint8_t mailbox = 0;
    unsigned acknowledgements = 0;
    const std::array finger{Point{123, 234, 2}};
    const std::array<Point, 0> empty{};
    auto poll = [&](int64_t now) {
        uint8_t status = 0;
        const auto result = ReadKorvoTouchReport(status,
            [&](uint8_t& value) { value = mailbox; return true; },
            [&] {
                ++acknowledgements;
                if ((mailbox & 0x0f) != 0) {
                    state.Update(std::span<const Point>(finger), now, 800, 480);
                } else {
                    state.Update(std::span<const Point>(empty), now, 800, 480);
                }
                mailbox = 0; // ACK consumes the report, not the physical finger.
                return true;
            });
        if (result == KorvoTouchReport::kNone) state.NoReport(now);
        assert(result != KorvoTouchReport::kError);
    };
    unsigned reports = 0;
    for (int64_t now = 6000; now < 3000000; now += 6000) {
        if (now == 6000 || now % 24000 == 0) {
            mailbox = 0x81; // Report cadence is slower than the 6 ms poll.
            ++reports;
        }
        poll(now);
        assert(state[0].pressed && state[0].id == 2);
    }
    assert(acknowledgements == reports);
    // Event-only firmware can leave a stationary contact unchanged for minutes.
    for (int64_t now = 3000000; now < 60000000; now += 6000) {
        poll(now);
        assert(state[0].pressed);
    }
    assert(acknowledgements == reports); // No writes to an unready mailbox.
    mailbox = 0x80; // A real zero-contact report releases on this very poll.
    poll(60000000);
    assert(!state[0].pressed && acknowledgements == reports + 1);
    poll(60006000);
    assert(!state[0].pressed);

    // Stale count bits without ready must not consume/ACK a partial report.
    mailbox = 0x01;
    poll(60012000);
    assert(!state[0].pressed && acknowledgements == reports + 1);

    for (unsigned value = 0; value < 256; ++value) {
        uint8_t status = 0;
        bool consumed = false;
        auto result = ReadKorvoTouchReport(status,
            [&](uint8_t& out) { out = value; return true; },
            [&] { consumed = true; return true; });
        assert(consumed == ((value & 0x80) != 0));
        const auto expected = !(value & 0x80) ? KorvoTouchReport::kNone :
            (value & 0x0f) <= 10 ? KorvoTouchReport::kReady : KorvoTouchReport::kError;
        assert(result == expected);
    }
    uint8_t status = 0;
    bool consumed = false;
    assert(ReadKorvoTouchReport(status,
        [](uint8_t&) { return false; },
        [&] { consumed = true; return true; }) == KorvoTouchReport::kError);
    assert(!consumed);
    assert(ReadKorvoTouchReport(status,
        [](uint8_t& out) { out = 0x81; return true; },
        [] { return false; }) == KorvoTouchReport::kError);
}

int main() {
    TestReportMailbox();
    KorvoTouchState<5> state;
    auto update = [&]<size_t N>(const std::array<Point, N>& points, int64_t now) {
        state.Update(std::span<const Point>(points), now, 800, 480);
    };
    const std::array<Point, 0> empty{};
    update(empty, 1000);
    for (size_t i = 0; i < 5; ++i) assert(!state[i].pressed);

    update(std::array{Point{123, 456, 7}}, 2000);
    assert(state[0].pressed && state[0].id == 7 && state[0].y == 456);
    update(std::array{Point{700, 20, 3}, Point{124, 455, 7}}, 3000);
    assert(state[0].id == 7 && state[0].x == 124);
    assert(state[1].pressed && state[1].id == 3);
    update(std::array{Point{125, 454, 7}, Point{699, 21, 3}}, 4000);
    assert(state[0].id == 7 && state[1].id == 3);
    update(std::array{Point{698, 22, 3}}, 5000);
    assert(!state[0].pressed && state[1].pressed);
    update(empty, 6000);
    for (size_t i = 0; i < 5; ++i) assert(!state[i].pressed);

    // Failed reports must not generate false taps or leave a stuck finger.
    update(std::array{Point{100, 200, 9}}, 7000);
    state.ReadFailed(156999);
    assert(state[0].pressed);
    state.ReadFailed(157000);
    assert(!state[0].pressed);
    update(std::array{Point{101, 201, 9}}, 200000);
    state.ReadFailed(201000);
    update(empty, 202000);
    assert(!state[0].pressed);  // A good release never waits for the timeout.

    update(std::array{Point{100, 200, 9}}, 202100);
    state.NoReport(10202100);
    state.ReadFailed(10208100);
    assert(state[0].pressed); // Error grace starts at the last healthy poll.
    state.ReadFailed(10352099);
    assert(state[0].pressed);
    state.ReadFailed(10352100);
    assert(!state[0].pressed);

    update(std::array{Point{800, 10, 1}, Point{10, 480, 2}}, 203000);
    for (size_t i = 0; i < 5; ++i) assert(!state[i].pressed);
    update(std::array{Point{799, 479, 1}, Point{20, 30, 1}}, 204000);
    assert(state[0].pressed && state[0].x == 799);
    for (size_t i = 1; i < 5; ++i) assert(!state[i].pressed);
    update(std::array{Point{800, 480, 1}}, 205000);
    assert(state[0].pressed && state[0].x == 799 && state[0].y == 479);
    update(std::array{Point{600, 300, 1}}, 206000);
    assert(state[0].pressed && state[0].x == 600);

    update(empty, 300000);
    update(std::array{Point{1, 1, 0}, Point{2, 2, 1}, Point{3, 3, 2},
                      Point{4, 4, 3}, Point{5, 5, 4}}, 301000);
    update(std::array{Point{6, 6, 5}, Point{5, 5, 4}, Point{4, 4, 3},
                      Point{3, 3, 2}, Point{2, 2, 1}}, 302000);
    assert(!state[0].pressed); // Release is visible before a slot changes ID.
    for (size_t i = 1; i < 5; ++i) assert(state[i].pressed && state[i].id == i);
    update(std::array{Point{6, 6, 5}, Point{5, 5, 4}, Point{4, 4, 3},
                      Point{3, 3, 2}, Point{2, 2, 1}}, 303000);
    assert(state[0].pressed && state[0].id == 5);
    for (size_t i = 1; i < 5; ++i) assert(state[i].id == i);

    // Compact persistent state, no heap or per-frame containers.
    static_assert(sizeof(KorvoTouchState<5>) <= 48);
    std::puts("Korvo touch: mailbox cadence, 60s stationary hold, immediate release, all status bytes,");
    std::puts("  errors, five contacts, reordered/duplicate IDs, bounds and fixed memory passed");
}
