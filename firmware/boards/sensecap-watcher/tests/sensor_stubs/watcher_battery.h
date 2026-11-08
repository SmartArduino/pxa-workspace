#pragma once
#include <cstdint>
class WatcherBattery {
public:
    bool present = true, read_ok = true;
    uint16_t mv = 3999;
    bool IsPresent() { return present; }
    bool GetVoltageMv(uint16_t* value) { *value = mv; return read_ok; }
};
