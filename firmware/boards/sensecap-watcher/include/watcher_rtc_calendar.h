// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>

namespace watcher_rtc {
constexpr bool Leap(unsigned year) { return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0); }
constexpr unsigned MonthDays(unsigned year, unsigned month) {
    constexpr unsigned lengths[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    return month >= 1 && month <= 12 ? lengths[month - 1] + (month == 2 && Leap(year)) : 0;
}
constexpr int Bcd(uint8_t value) {
    return (value & 15) <= 9 && (value >> 4) <= 9 ? (value >> 4) * 10 + (value & 15) : -1;
}
constexpr uint8_t ToBcd(unsigned value) { return uint8_t((value / 10) * 16 + value % 10); }
// PXA uses UTC, independent of the system display timezone. The century flag
// is defined here as 0=2000..2099, matching Seeed's time setting convention.
inline bool Decode(const uint8_t (&raw)[7], int64_t* seconds) {
    if (!seconds || (raw[0] & 0x80) || (raw[5] & 0x80)) return false;
    int second = Bcd(raw[0] & 0x7f), minute = Bcd(raw[1] & 0x7f), hour = Bcd(raw[2] & 0x3f);
    int day = Bcd(raw[3] & 0x3f), month = Bcd(raw[5] & 0x1f), year = Bcd(raw[6]);
    if (second < 0 || second > 59 || minute < 0 || minute > 59 || hour < 0 || hour > 23 ||
        year < 0 || month < 1 || month > 12 || day < 1 || unsigned(day) > MonthDays(2000 + year, month)) return false;
    int64_t days = 10957; // 2000-01-01 minus 1970-01-01.
    for (int y = 2000; y < 2000 + year; ++y) days += Leap(y) ? 366 : 365;
    for (int m = 1; m < month; ++m) days += MonthDays(2000 + year, m);
    *seconds = (days + day - 1) * 86400 + hour * 3600 + minute * 60 + second;
    return true;
}
inline bool Encode(int64_t seconds, uint8_t (&raw)[7]) {
    if (seconds < INT64_C(946684800) || seconds >= INT64_C(4102444800)) return false;
    int64_t days = seconds / 86400 - 10957;
    unsigned year = 2000, month = 1;
    while (days >= (Leap(year) ? 366 : 365)) { days -= Leap(year) ? 366 : 365; ++year; }
    while (days >= MonthDays(year, month)) { days -= MonthDays(year, month); ++month; }
    raw[0] = ToBcd(unsigned(seconds % 60));
    raw[1] = ToBcd(unsigned(seconds / 60 % 60));
    raw[2] = ToBcd(unsigned(seconds / 3600 % 24));
    raw[3] = ToBcd(unsigned(days + 1));
    raw[4] = uint8_t((seconds / 86400 + 4) % 7);
    raw[5] = ToBcd(month);
    raw[6] = ToBcd(year - 2000);
    return true;
}
}
