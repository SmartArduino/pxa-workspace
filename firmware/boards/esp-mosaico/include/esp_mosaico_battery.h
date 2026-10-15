#pragma once

#include <atomic>
#include <cstdint>

// Read-only BQ27220 monitor. The input worker calls Poll; UI reads one atomic
// cached snapshot and never performs I2C. No profile, unseal or flash writes.
class EspMosaicoBattery {
public:
    struct Snapshot { bool valid; uint8_t percent; bool charging; };

    template <typename Read>
    void Poll(uint32_t now_ms, Read read) {
        if (attempted_ && now_ms - last_attempt_ms_ < 2000) return;
        attempted_ = true;
        last_attempt_ms_ = now_ms;
        uint8_t registers[6], soc[2];
        if (read(0x08, registers, sizeof(registers)) && read(0x2c, soc, sizeof(soc))) {
            const uint16_t voltage = Word(registers);
            const uint16_t status = Word(registers + 2);
            const uint16_t raw_current = Word(registers + 4);
            const int32_t current = raw_current < 32768 ? raw_current : int32_t(raw_current) - 65536;
            const uint16_t percent = Word(soc);
            if (status != UINT16_MAX && (status & (1u << 3)) == 0) {
                cached_.store(0, std::memory_order_release); // Gauge reports no battery.
                return;
            }
            if (voltage >= 2000 && voltage <= 5000 && percent <= 100 &&
                status != UINT16_MAX) {
                const bool charging = current > 5 && (status & (1u << 9)) == 0;
                cached_.store(0x100u | percent | (charging ? 0x200u : 0), std::memory_order_release);
                last_valid_ms_ = now_ms;
                return;
            }
        }
        // Brief bus failures preserve the last good reading; persistent errors
        // hide the battery instead of presenting an indefinitely stale value.
        if (now_ms - last_valid_ms_ >= 10000) cached_.store(0, std::memory_order_release);
    }

    Snapshot Get() const {
        const uint32_t cached = cached_.load(std::memory_order_acquire);
        return {bool(cached & 0x100), static_cast<uint8_t>(cached & 0x7f), bool(cached & 0x200)};
    }

private:
    static uint16_t Word(const uint8_t* bytes) { return bytes[0] | uint16_t(bytes[1]) << 8; }
    std::atomic<uint32_t> cached_{0};
    uint32_t last_attempt_ms_ = 0;
    uint32_t last_valid_ms_ = 0;
    bool attempted_ = false;
};
