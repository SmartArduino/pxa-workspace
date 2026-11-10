// SPDX-License-Identifier: Apache-2.0
#pragma once
#ifdef ESP_PLATFORM
#include <sys/lock.h>
#else
#include <mutex>
#endif

namespace espp4_a {
// IDF serializes individual commands. The shared controller must also retain
// a slot across CMD55/ACMD pairs, initialization, and data/status sequences.
#ifdef ESP_PLATFORM
inline _lock_t sd_bus_mutex;
#else
inline std::recursive_mutex sd_bus_mutex;
#endif
class SdBusGuard {
public:
    SdBusGuard() {
#ifdef ESP_PLATFORM
        _lock_acquire_recursive(&sd_bus_mutex);
#else
        sd_bus_mutex.lock();
#endif
    }
    ~SdBusGuard() {
#ifdef ESP_PLATFORM
        _lock_release_recursive(&sd_bus_mutex);
#else
        sd_bus_mutex.unlock();
#endif
    }
    SdBusGuard(const SdBusGuard&) = delete;
    SdBusGuard& operator=(const SdBusGuard&) = delete;
};
}
