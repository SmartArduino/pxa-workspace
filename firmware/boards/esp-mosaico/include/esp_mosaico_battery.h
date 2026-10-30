#pragma once

#include <atomic>
#include <cstdint>

// Read-only BQ27220 monitor. The input worker calls Poll; UI reads one atomic
// cached snapshot and never performs I2C. No profile, unseal or flash writes.
class EspMosaicoBattery {
public:
    struct Snapshot {
        bool valid;
        uint8_t percent;
        bool charging;
        uint16_t voltage_mv;
        int16_t current_ma;
        bool temperature_valid;
        int32_t temperature_milli_celsius;
    };

    template <typename Read>
    void Poll(uint32_t now_ms, Read read) {
        if (attempted_ && now_ms - last_attempt_ms_ < 2000) return;
        attempted_ = true;
        last_attempt_ms_ = now_ms;
        uint8_t registers[8], soc[2];
        if (read(0x06, registers, sizeof(registers)) && read(0x2c, soc, sizeof(soc))) {
            const uint16_t temperature = Word(registers);
            const uint16_t voltage = Word(registers + 2);
            const uint16_t status = Word(registers + 4);
            const uint16_t raw_current = Word(registers + 6);
            const int32_t current = raw_current < 32768 ? raw_current : int32_t(raw_current) - 65536;
            const uint16_t percent = Word(soc);
            if (status != UINT16_MAX && (status & (1u << 3)) == 0) {
                cached_.store(0, std::memory_order_release); // Gauge reports no battery.
                return;
            }
            if (voltage >= 2000 && voltage <= 5000 && percent <= 100 &&
                status != UINT16_MAX) {
                const bool charging = current > 5 && (status & (1u << 9)) == 0;
                sequence_.fetch_add(1, std::memory_order_acq_rel);
                electrical_.store(uint32_t(voltage) | (uint32_t(raw_current) << 16), std::memory_order_relaxed);
                temperature_.store(temperature, std::memory_order_relaxed);
                cached_.store(0x100u | percent | (charging ? 0x200u : 0), std::memory_order_release);
                sequence_.fetch_add(1, std::memory_order_release);
                last_valid_ms_ = now_ms;
                return;
            }
        }
        // Brief bus failures preserve the last good reading; persistent errors
        // hide the battery instead of presenting an indefinitely stale value.
        if (now_ms - last_valid_ms_ >= 10000) cached_.store(0, std::memory_order_release);
    }

    Snapshot Get() const {
        for (unsigned attempt = 0; attempt < 3; ++attempt) {
            const uint32_t sequence = sequence_.load(std::memory_order_acquire);
            if (sequence & 1) continue;
            const uint32_t cached = cached_.load(std::memory_order_acquire);
            const uint32_t electrical = electrical_.load(std::memory_order_relaxed);
            const uint32_t temperature = temperature_.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (sequence != sequence_.load(std::memory_order_acquire)) continue;
            if (!(cached & 0x100)) return {};
            const uint16_t raw_current = electrical >> 16;
            const int32_t current = raw_current < 32768 ? raw_current : int32_t(raw_current) - 65536;
            const bool temperature_valid = temperature >= 2331 && temperature <= 3981;
            return {true, static_cast<uint8_t>(cached & 0x7f), bool(cached & 0x200),
                static_cast<uint16_t>(electrical), static_cast<int16_t>(current), temperature_valid,
                temperature_valid ? int32_t(temperature) * 100 - 273150 : 0};
        }
        return {}; // Never wait for the polling task from a UI/Host callback.
    }

private:
    static uint16_t Word(const uint8_t* bytes) { return bytes[0] | uint16_t(bytes[1]) << 8; }
    std::atomic<uint32_t> cached_{0};
    std::atomic<uint32_t> sequence_{0}, electrical_{0}, temperature_{0};
    uint32_t last_attempt_ms_ = 0;
    uint32_t last_valid_ms_ = 0;
    bool attempted_ = false;
};
