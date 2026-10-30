// SPDX-License-Identifier: MIT
#include "watcher_rtc_calendar.h"
#include <cassert>
#include <cstdio>
int main() {
    uint8_t raw[7];
    int64_t decoded;
    for (int64_t stamp = 946684800; stamp < 4102444800; stamp += 86400) {
        assert(watcher_rtc::Encode(stamp + 86399, raw));
        assert(watcher_rtc::Decode(raw, &decoded) && decoded == stamp + 86399);
    }
    assert(!watcher_rtc::Encode(946684799, raw));
    assert(!watcher_rtc::Encode(4102444800, raw));
    assert(watcher_rtc::Encode(1709164800, raw)); // 2024-02-29 UTC
    assert(raw[3] == 0x29 && raw[5] == 2 && raw[6] == 0x24);
    raw[0] |= 0x80; assert(!watcher_rtc::Decode(raw, &decoded)); raw[0] &= 0x7f;
    raw[6] = 0x23; assert(!watcher_rtc::Decode(raw, &decoded)); raw[6] = 0x24;
    raw[1] = 0x6a; assert(!watcher_rtc::Decode(raw, &decoded)); raw[1] = 0;
    raw[5] |= 0x80; assert(!watcher_rtc::Decode(raw, &decoded));
    puts("Watcher RTC: all dates 2000..2099, leap days, invalid BCD, low voltage and century limits passed");
}
