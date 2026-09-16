#include "watcher_battery.h"

#include "sensecap_watcher_config.h"
#include "watcher_io_expander.h"

#include <esp_log.h>

namespace {
constexpr char kTag[] = "WatcherBattery";
constexpr int kSamplesPerReading = 10;

// Factory discharge curve for the single-cell battery: a parabola fitted over
// the 3.4 V - 4.2 V usable range. Voltage is in millivolts.
int32_t VoltageToPercent(int32_t millivolts) {
    const int64_t quadratic = -static_cast<int64_t>(millivolts) * millivolts +
                              9016LL * millivolts - 19189000LL;
    int32_t percent = static_cast<int32_t>(quadratic / 10000);
    if (percent > 100) percent = 100;
    if (percent < 0) percent = 0;
    return percent;
}
}  // namespace

bool WatcherBattery::Initialize(WatcherIoExpander* expander,
                                adc_oneshot_unit_handle_t adc) {
    if (expander == nullptr || adc == nullptr) return false;
    expander_ = expander;
    adc_ = adc;

    const adc_oneshot_chan_cfg_t channel_config = {
        .atten = ADC_ATTEN_DB_2_5,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_oneshot_config_channel(adc_, WATCHER_BATTERY_ADC_CHANNEL,
                                   &channel_config) != ESP_OK) {
        ESP_LOGE(kTag, "Cannot configure battery ADC channel");
        return false;
    }

    const adc_cali_curve_fitting_config_t calibration_config = {
        .unit_id = WATCHER_BATTERY_ADC_UNIT,
        .chan = WATCHER_BATTERY_ADC_CHANNEL,
        .atten = ADC_ATTEN_DB_2_5,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&calibration_config,
                                             &calibration_) != ESP_OK) {
        calibration_ = nullptr;
        return false;
    }
    uint16_t voltage = 0;
    ESP_LOGI(kTag, "Battery monitor ready (first sample %d mV)",
             GetVoltageMv(&voltage) ? voltage : 0);
    return true;
}

bool WatcherBattery::IsCharging() {
    if (expander_ == nullptr) return false;
    bool level = false;
    if (!expander_->InputLevel(WATCHER_IO_CHRG_DET, &level)) {
        return false;
    }
    return level;
}

bool WatcherBattery::IsPresent() {
    if (expander_ == nullptr) return false;
    bool level = false;
    if (!expander_->InputLevel(WATCHER_IO_PWR_BAT_DET, &level)) {
        return false;
    }
    return !level;
}

bool WatcherBattery::GetVoltageMv(uint16_t* millivolts) {
    if (millivolts == nullptr || adc_ == nullptr || calibration_ == nullptr) {
        return false;
    }
    uint32_t total = 0;
    for (int index = 0; index < kSamplesPerReading; ++index) {
        int raw = 0;
        int calibrated = 0;
        if (adc_oneshot_read(adc_, WATCHER_BATTERY_ADC_CHANNEL, &raw) !=
                ESP_OK ||
            adc_cali_raw_to_voltage(calibration_, raw, &calibrated) != ESP_OK) {
            return false;
        }
        const int divided =
            static_cast<int>(calibrated * WATCHER_BATTERY_DIVIDER_GAIN + 0.5f);
        total += static_cast<uint32_t>(divided);
    }
    *millivolts = static_cast<uint16_t>(total / kSamplesPerReading);
    return true;
}

bool WatcherBattery::GetPercent(uint8_t* percent) {
    if (percent == nullptr) return false;
    uint16_t voltage = 0;
    if (!GetVoltageMv(&voltage)) return false;
    *percent = static_cast<uint8_t>(VoltageToPercent(voltage));
    return true;
}
