// SPDX-License-Identifier: Apache-2.0
#include "espp4_a_touch.h"
#include <pxadb_pointer_samples.h>
#include <algorithm>
#include <cassert>
#include <cstdio>

int main() {
    {
        espp4_a::TouchSchedule schedule;
        assert(schedule.Due(1000, false, false));
        schedule.Begin(1000); schedule.Complete(true);
        assert(!schedule.Due(1499, false, false));
        assert(schedule.Due(1016, false, false, false)); // Poll until IRQ wiring proves active.
        assert(schedule.Due(1500, false, false));
        assert(!schedule.Due(1015, true, false));
        assert(schedule.Due(1016, true, false));
        assert(schedule.Due(1016, false, true));
        schedule.Request(); assert(schedule.Due(1005, false, false));
        schedule.Begin(1005);
        schedule.Request(); // A second IRQ during a read must survive it.
        schedule.Complete(true); assert(schedule.Due(1010, false, false));
        schedule.Begin(1010); schedule.Complete(false);
        assert(schedule.Failed() && !schedule.Due(1019, true, true));
        assert(schedule.Due(1020, false, false));
        schedule.Begin(1020); schedule.Complete(false);
        assert(!schedule.Due(1039, true, true) && schedule.Due(1040, true, true));
        for (unsigned i=0;i<10;++i) {schedule.Begin(2000);schedule.Complete(false);}
        assert(!schedule.Due(2159, true, true) && schedule.Due(2160, false, false));
        schedule.Begin(UINT32_MAX-4); schedule.Complete(true);
        schedule.Request(); assert(schedule.Due(0, false, false));
        assert(!schedule.Failed());
    }
    std::array<uint8_t, 35> bytes{};
    auto finger = [&](unsigned slot, uint16_t x, uint16_t y, bool down) {
        auto* p = bytes.data() + slot * 7;
        p[0] = uint8_t((down ? 0x80 : 0) | 0x40 | (x >> 8));
        p[1] = uint8_t(x); p[2] = uint8_t(y >> 8); p[3] = uint8_t(y);
    };
    espp4_a::TouchReport report{};
    for (unsigned i = 0; i < 5; ++i) finger(i, 20 + i * 100, 40 + i * 190, true);
    assert(espp4_a::DecodeTouch(bytes, 480, 854, report));
    for (unsigned i = 0; i < 5; ++i)
        assert(report[i].pressed && report[i].x == 20 + i * 100 && report[i].y == 40 + i * 190);
    // Lifting the first finger must not transfer the other fingers to slot 0.
    finger(0, 20, 40, false);
    assert(espp4_a::DecodeTouch(bytes, 480, 854, report));
    assert(!report[0].pressed && report[1].pressed && report[1].x == 120);
    // Crossing coordinates keeps controller identity, rather than nearest XY.
    finger(1, 420, 800, true); finger(4, 120, 230, true);
    assert(espp4_a::DecodeTouch(bytes, 480, 854, report));
    assert(report[1].x == 420 && report[4].x == 120);
    finger(2, 479, 853, true);
    assert(espp4_a::DecodeTouch(bytes, 480, 854, report));
    auto previous = report;
    finger(2, 480, 853, true);
    assert(!espp4_a::DecodeTouch(bytes, 480, 854, report));
    assert(report[2].x == previous[2].x && report[4].pressed);
    assert(!espp4_a::DecodeTouch(std::span<const uint8_t>(bytes.data(), 34), 480, 854, report));
    std::array<uint8_t, 42> oversized{};
    assert(!espp4_a::DecodeTouch(oversized, 480, 854, report));
    finger(2, 200, 300, true);
    unsigned reads = 0;
    auto read = [&](int reg, uint8_t* output, size_t n) {
        ++reads;
        if (reg == 0x10) { assert(n == 1); *output = 8; }
        else { assert(reg == 0x14 && n == 35); std::copy(bytes.begin(), bytes.end(), output); }
        return true;
    };
    assert(espp4_a::ReadTouch(read, 5, 480, 854, report) && reads == 2);
    // I2C failure preserves the snapshot for the board's bounded grace period.
    assert(!espp4_a::ReadTouch([](int, uint8_t*, size_t) { return false; }, 5, 480, 854, report));
    assert(report[1].pressed && report[4].pressed);
    assert(espp4_a::ReadTouch([](int reg, uint8_t* p, size_t n) {
        assert(reg == 0x10 && n == 1); *p = 0; return true;
    }, 5, 480, 854, report));
    for (const auto& p : report) assert(!p.pressed);
    pxadb::PointerSamples<5> samples;
    for (unsigned i = 0; i < 5; ++i) assert(samples.Push(i, i * 50, 600, true));
    for (unsigned i = 0; i < 5; ++i) {
        bool more;
        assert(samples.Read(i, more).pressed && !more);
    }
    assert(samples.Push(0, 0, 600, false));
    for (unsigned i = 1; i < 5; ++i) {
        bool more; assert(samples.Read(i, more).pressed);
    }
    for (unsigned i = 0; i < 5; ++i) {
        assert(samples.Cancel(i)); assert(samples.Push(i, 200, 500, true));
        bool more;
        const auto cancelled = samples.Read(i, more);
        assert(cancelled.cancel && !cancelled.pressed && more);
        assert(samples.Read(i, more).pressed && !more);
    }
    assert(!samples.Push(5, 0, 0, true));
    puts("ESPP4-A: five fixed touch IDs, crossing/lifting, malformed reports, I2C errors and independent cancellation passed");
}
