// SPDX-License-Identifier: Apache-2.0
#include "doit_s31_touch.h"
#include <pxadb_pointer_samples.h>
#include <algorithm>
#include <cassert>
#include <cstdio>

int main() {
    std::array<uint8_t, 35> bytes{};
    auto finger = [&](unsigned slot, uint16_t x, uint16_t y, bool down) {
        auto* p = bytes.data() + slot * 7;
        p[0] = uint8_t((down ? 0x80 : 0) | 0x40 | (x >> 8));
        p[1] = uint8_t(x); p[2] = uint8_t(y >> 8); p[3] = uint8_t(y);
    };
    doit_s31::TouchReport report{};
    for (unsigned i = 0; i < 5; ++i) finger(i, 20 + i * 100, 40 + i * 190, true);
    assert(doit_s31::DecodeTouch(bytes, 480, 854, report));
    for (unsigned i = 0; i < 5; ++i)
        assert(report[i].pressed && report[i].x == 20 + i * 100 && report[i].y == 40 + i * 190);
    // Lifting the first finger must not transfer the other fingers to slot 0.
    finger(0, 20, 40, false);
    assert(doit_s31::DecodeTouch(bytes, 480, 854, report));
    assert(!report[0].pressed && report[1].pressed && report[1].x == 120);
    // Crossing coordinates keeps controller identity, rather than nearest XY.
    finger(1, 420, 800, true); finger(4, 120, 230, true);
    assert(doit_s31::DecodeTouch(bytes, 480, 854, report));
    assert(report[1].x == 420 && report[4].x == 120);
    finger(2, 479, 853, true);
    assert(doit_s31::DecodeTouch(bytes, 480, 854, report));
    auto previous = report;
    finger(2, 480, 853, true);
    assert(!doit_s31::DecodeTouch(bytes, 480, 854, report));
    assert(report[2].x == previous[2].x && report[4].pressed);
    assert(!doit_s31::DecodeTouch(std::span<const uint8_t>(bytes.data(), 34), 480, 854, report));
    std::array<uint8_t, 77> oversized{};
    assert(!doit_s31::DecodeTouch(oversized, 480, 854, report));
    finger(2, 200, 300, true);
    unsigned reads = 0;
    auto read = [&](int reg, uint8_t* output, size_t n) {
        ++reads;
        if (reg == 0x10) { assert(n == 1); *output = 8; }
        else { assert(reg == 0x14 && n == 35); std::copy(bytes.begin(), bytes.end(), output); }
        return true;
    };
    assert(doit_s31::ReadTouch(read, 5, 480, 854, report) && reads == 2);
    // I2C failure preserves the snapshot for the board's bounded grace period.
    assert(!doit_s31::ReadTouch([](int, uint8_t*, size_t) { return false; }, 5, 480, 854, report));
    assert(report[1].pressed && report[4].pressed);
    assert(doit_s31::ReadTouch([](int reg, uint8_t* p, size_t n) {
        assert(reg == 0x10 && n == 1); *p = 0; return true;
    }, 5, 480, 854, report));
    for (const auto& p : report) assert(!p.pressed);
    std::array<uint8_t, 70> ten{};
    for (unsigned i = 0; i < 10; ++i) {
        ten[i*7] = 0x81; ten[i*7+1] = 0xdf; // x=479
        ten[i*7+2] = 0xc3; ten[i*7+3] = 0x55; // y=853
    }
    assert(doit_s31::DecodeTouch(ten, 480, 854, report));
    for (const auto& p : report) assert(p.pressed && p.x == 479 && p.y == 853);
    assert(!doit_s31::ReadTouch(read, 11, 480, 854, report));
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
    puts("DOIT-S31: ten fixed touch IDs, crossing/lifting, malformed reports, I2C errors and independent cancellation passed");
}
