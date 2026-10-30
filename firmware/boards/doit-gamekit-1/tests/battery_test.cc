#include "doit_gamekit_1_battery.h"
#include <esp_adc/adc_cali_scheme.h>
#include <array>
#include <cassert>

namespace {
int failure_stage = 0, live_units = 0, live_calibrations = 0;
int reads = 0, conversions = 0, read_failure = -1, conversion_failure = -1;
int raw_rail_at = -1;
std::array<int, 17> voltages;
void Samples(int mv) {
    voltages.fill(mv);
    reads = conversions = 0;
    read_failure = conversion_failure = -1;
    raw_rail_at = -1;
}
}

esp_err_t adc_oneshot_io_to_channel(int gpio, adc_unit_t* unit, adc_channel_t* channel) {
    assert(gpio == 49);
    *unit = ADC_UNIT_1;
    *channel = ADC_CHANNEL_7;
    return failure_stage == 1 ? ESP_FAIL : ESP_OK;
}
esp_err_t adc_oneshot_new_unit(const adc_oneshot_unit_init_cfg_t* config, adc_oneshot_unit_handle_t* handle) {
    assert(config->unit_id == ADC_UNIT_1);
    if (failure_stage == 2) return ESP_FAIL;
    ++live_units;
    *handle = reinterpret_cast<void*>(1);
    return ESP_OK;
}
esp_err_t adc_oneshot_config_channel(adc_oneshot_unit_handle_t handle, adc_channel_t channel,
                                    const adc_oneshot_chan_cfg_t* config) {
    assert(handle && channel == ADC_CHANNEL_7 && config->atten == ADC_ATTEN_DB_0);
    assert(config->bitwidth == ADC_BITWIDTH_DEFAULT);
    return failure_stage == 3 ? ESP_FAIL : ESP_OK;
}
esp_err_t adc_cali_create_scheme_line_fitting(const adc_cali_line_fitting_config_t* config,
                                            adc_cali_handle_t* handle) {
    assert(config->unit_id == ADC_UNIT_1 && config->atten == ADC_ATTEN_DB_0);
    assert(config->bitwidth == ADC_BITWIDTH_DEFAULT);
    if (failure_stage == 4) return ESP_FAIL;
    ++live_calibrations;
    *handle = reinterpret_cast<void*>(2);
    return ESP_OK;
}
esp_err_t adc_oneshot_del_unit(adc_oneshot_unit_handle_t handle) {
    assert(handle && live_units > 0);
    --live_units;
    return ESP_OK;
}
esp_err_t adc_cali_delete_scheme_line_fitting(adc_cali_handle_t handle) {
    assert(handle && live_calibrations > 0);
    --live_calibrations;
    return ESP_OK;
}
esp_err_t adc_oneshot_read(adc_oneshot_unit_handle_t handle, adc_channel_t channel, int* raw) {
    assert(handle && channel == ADC_CHANNEL_7 && reads < 17);
    if (reads++ == read_failure) return ESP_ERR_TIMEOUT;
    // S31 results are comparator vectors, not a linear 12-bit voltage code.
    *raw = reads - 1 == raw_rail_at ? 0x1ffff : 0x11000 + reads - 1;
    return ESP_OK;
}
esp_err_t adc_cali_chan_raw_to_voltage(adc_cali_handle_t handle, adc_channel_t channel,
                                      int raw, int* mv) {
    assert(handle && channel == ADC_CHANNEL_7 && raw >= 0x11000);
    if (conversions++ == conversion_failure) return ESP_FAIL;
    *mv = voltages.at(reads - 1);
    return ESP_OK;
}

int main() {
    {
        DoitGamekit1Battery battery;
        assert(!battery.Get().valid);
        assert(!battery.Initialize(0, 100));
        assert(!battery.Initialize(100, 0));
        for (failure_stage = 1; failure_stage <= 4; ++failure_stage) {
            assert(!battery.Initialize(100, 100));
            assert(!battery.Get().valid && live_units == 0 && live_calibrations == 0);
        }
        failure_stage = 0;
        assert(battery.Initialize(100, 100));
        Samples(1900);
        voltages[0] = 2100; // The settling conversion must not affect the result.
        battery.Poll(0);
        assert(reads == 17 && conversions == 17);
        assert(battery.Get().valid && battery.Get().voltage_mv == 3800 && battery.Get().percent == 45);
        Samples(1950);
        battery.Poll(2000);
        assert(battery.Get().voltage_mv == 3825); // Suppress load/noise jumps.
        Samples(1900);
        voltages[8] = 1980; // One clipped sample invalidates the whole reading.
        battery.Poll(4000);
        assert(!battery.Get().valid);
        Samples(1900);
        battery.Poll(6000);
        assert(battery.Get().valid && battery.Get().voltage_mv == 3800);
        Samples(2100); // Original 100k/100k board cannot measure a full cell.
        battery.Poll(8000);
        assert(!battery.Get().valid);
        Samples(1900);
        raw_rail_at = 8; // Calibration can shift the saturated voltage below 1.98 V.
        battery.Poll(9000);
        assert(!battery.Get().valid);
        Samples(100); // Missing/unusable battery voltage clears the cached value.
        battery.Poll(10000);
        assert(!battery.Get().valid);

        assert(battery.Initialize(120, 100));
        Samples(1909); // Corrected divider covers a 4.2 V cell.
        battery.Poll(12000);
        assert(battery.Get().valid && battery.Get().voltage_mv == 4200 && battery.Get().percent == 100);
        Samples(1909);
        read_failure = 4;
        battery.Poll(14000);
        assert(battery.Get().valid && battery.Get().percent == 100);
        Samples(1909);
        conversion_failure = 9;
        battery.Poll(21999);
        assert(battery.Get().valid);
        Samples(1909);
        read_failure = 0;
        battery.Poll(22000);
        assert(!battery.Get().valid);
        Samples(1500);
        battery.Poll(24000);
        assert(battery.Get().valid && battery.Get().voltage_mv == 3300 && battery.Get().percent == 0);

        int previous = -1;
        for (int mv = 1250; mv <= 1977; ++mv) {
            assert(battery.Initialize(120, 100));
            Samples(mv);
            battery.Poll(0);
            const auto value = battery.Get();
            assert(value.valid && value.percent >= previous && value.percent <= 100);
            previous = value.percent;
        }
        assert(previous == 100);
        assert(battery.Initialize(200, 100));
        Samples(1500);
        battery.Poll(0);
        assert(!battery.Get().valid); // A 4.5 V cell reading is implausible.
        assert(battery.Initialize(100, 100));
        Samples(1900);
        battery.Poll(UINT32_MAX - 5000);
        Samples(1900);
        read_failure = 0;
        battery.Poll(4998);
        assert(battery.Get().valid);
        Samples(1900);
        read_failure = 0;
        battery.Poll(4999);
        assert(!battery.Get().valid); // The timeout works across clock wrap.
    }
    assert(live_units == 0 && live_calibrations == 0);
}
