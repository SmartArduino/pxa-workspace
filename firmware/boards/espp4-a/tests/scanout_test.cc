// SPDX-License-Identifier: Apache-2.0
#include "espp4_a_scanout.h"
#include <cassert>
#include <cstdio>

int main() {
    espp4_a::ScanoutClock clock;
    uint64_t time = 0;
    const auto first = clock.Fence();
    assert(!clock.Latched(first));
    assert(!clock.Timestamp(first, 100, time));
    clock.Complete(100);
    assert(clock.Latched(first)); // Old buffer reusable during the new scan.
    assert(!clock.Timestamp(first, 150, time)); // Latched, not fully scanned.
    const auto second = clock.Fence();
    assert(!clock.Latched(second));
    clock.Complete(200);
    assert(clock.Latched(second));
    assert(clock.Timestamp(first, 250, time) && time == 200);
    assert(!clock.Timestamp(second, 250, time));
    clock.Complete(300);
    assert(clock.Timestamp(second, 350, time) && time == 300);
    // Late collection retains the actual EOF, not the task wake-up time.
    assert(clock.Timestamp(first, 1000, time) && time == 200);
    for (unsigned i = 4; i <= 70; ++i) clock.Complete(i * 100);
    assert(clock.Timestamp(first, 8000, time) && time == 7000);
    // DMA event counter and ISR microsecond timestamp wrap independently.
    espp4_a::ScanoutClock wrap(UINT32_MAX - 1);
    const auto fence = wrap.Fence();
    assert(!wrap.Latched(fence));
    wrap.Complete(UINT32_MAX - 10);
    assert(wrap.Latched(fence));
    assert(!wrap.Timestamp(fence, UINT64_C(0x100000020), time));
    wrap.Complete(16);
    assert(wrap.Timestamp(fence, UINT64_C(0x100000020), time) && time == UINT64_C(0x100000010));
    const auto next = wrap.Fence();
    wrap.Complete(32); wrap.Complete(48);
    assert(wrap.Timestamp(next, UINT64_C(0x100000040), time) && time == UINT64_C(0x100000030));
    espp4_a::ScanoutClock timing;
    timing.Complete(UINT32_MAX - 10000);
    timing.Complete(6500);
    timing.Complete(41500);
    const auto stats = timing.TakeStats();
    assert(stats.scans == 3 && stats.delayed == 1 && stats.max_interval_us == 35000);
    assert(timing.TakeStats().scans == 0);
    timing.Complete(58000);
    const auto next_stats = timing.TakeStats();
    assert(next_stats.scans == 1 && next_stats.delayed == 0 && next_stats.max_interval_us == 16500);
    puts("ESPP4-A: DMA completion fences, scan-stall counters and timestamp/counter wraps passed");
}
