#pragma once

#include <cstdint>

#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_oneshot.h>

class WatcherIoExpander;

// SenseCAP Watcher battery gauge: 62k/20k divider into ADC1 channel 2 and the
// charge/battery-present detect pins on the PCA9555.
class WatcherBattery {
public:
    bool Initialize(WatcherIoExpander* expander, adc_oneshot_unit_handle_t adc);
    bool IsCharging();
    bool IsPresent();
    // Average ten calibrated ADC samples and apply the Watcher discharge curve.
    bool GetPercent(uint8_t* percent);
    bool GetVoltageMv(uint16_t* millivolts);

private:
    WatcherIoExpander* expander_ = nullptr;
    adc_oneshot_unit_handle_t adc_ = nullptr;
    adc_cali_handle_t calibration_ = nullptr;
};
