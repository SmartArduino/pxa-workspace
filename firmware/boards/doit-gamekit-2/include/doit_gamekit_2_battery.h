#pragma once

#include <atomic>
#include <cstdint>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_oneshot.h>

// Single-cell voltage estimate, not a coulomb counter. Only the battery worker
// touches ADC/calibration state; the UI reads an atomic cached snapshot.
class DoitGamekit2Battery {
public:
    struct Snapshot {
        bool valid;
        uint8_t percent;
        uint16_t voltage_mv;
        bool usb_power_valid;
        bool usb_power_connected;
    };

    ~DoitGamekit2Battery();
    bool Initialize(uint32_t upper_kohm, uint32_t lower_kohm,
                    uint32_t gain_permille = 1089, int32_t offset_mv = 127);
    void PollPower(uint32_t now_ms);
    void Poll(uint32_t now_ms);
    Snapshot Get() const;

private:
    void Release();
    adc_oneshot_unit_handle_t adc_ = nullptr;
    adc_cali_handle_t calibration_ = nullptr;
    adc_channel_t channel_ = ADC_CHANNEL_7;
    uint32_t upper_kohm_ = 120, lower_kohm_ = 100;
    uint32_t gain_permille_ = 1089;
    int32_t offset_mv_ = 127;
    bool usb_ready_ = false, usb_seen_ = false, usb_candidate_ = false;
    uint32_t usb_changed_ms_ = 0;
    uint32_t last_valid_ms_ = 0;
    int filtered_mv_ = 0;
    bool sample_failed_ = false, out_of_range_ = false;
    std::atomic<uint32_t> cached_{0};
};
