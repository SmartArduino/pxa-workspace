#include "wifi_rssi_cache.h"
#include <cassert>

int main() {
    WifiRssiCache cache;
    int calls = 0, rssi = -50;
    auto read = [&] { ++calls; return rssi; };
    assert(cache.Get(0, read) == -50 && calls == 1);
    rssi = -80;
    for (int64_t now = 1; now < WifiRssiCache::kSampleIntervalUs; now += 10000)
        assert(cache.Get(now, read) == -50 && calls == 1);
    assert(cache.Get(5000000, read) == -80 && calls == 2);
    cache.Reset(); // Disconnect/reconnect must not wait for the next interval.
    rssi = -60;
    assert(cache.Get(5100000, read) == -60 && calls == 3);
    rssi = 0; // Driver failure cannot look like full signal.
    assert(cache.Get(10100000, read) == -60 && calls == 4);
    cache.Reset();
    assert(cache.Get(10200000, read) == WifiRssiCache::kUnavailable && calls == 5);
    rssi = -70;
    assert(cache.Get(1, read) == -70 && calls == 6); // Monotonic epoch reset.
}
