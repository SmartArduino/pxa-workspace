#ifndef ADC_BATTERY_MONITOR_H
#define ADC_BATTERY_MONITOR_H

#include <functional>
#include <driver/gpio.h>
#include <adc_battery_estimation.h>
#include <esp_timer.h>

class AdcBatteryMonitor {
public:
    AdcBatteryMonitor(adc_unit_t adc_unit, adc_channel_t adc_channel,
                      float upper_resistor, float lower_resistor,
                      gpio_num_t charging_pin = GPIO_NUM_NC,
                      adc_oneshot_unit_handle_t adc_handle = nullptr);
    ~AdcBatteryMonitor();

    bool IsCharging();
    bool IsDischarging();
    bool GetBatteryLevel(uint8_t& level);
    uint8_t GetBatteryLevel();

    void OnChargingStatusChanged(std::function<void(bool)> callback);

private:
    gpio_num_t charging_pin_;
    adc_oneshot_unit_handle_t adc_handle_ = nullptr;
    adc_channel_t adc_channel_;
    adc_battery_estimation_handle_t adc_battery_estimation_handle_ = nullptr;
    adc_cali_handle_t adc_cali_handle_ = nullptr;
    float voltage_divider_gain_ = 0.0f;
    bool owns_adc_calibration_ = false;
    esp_timer_handle_t timer_handle_ = nullptr;
    bool is_charging_ = false;
    uint8_t last_battery_level_ = 0;
    bool has_valid_battery_level_ = false;
    int64_t last_battery_sample_time_us_ = 0;
    int64_t last_diagnostic_log_time_us_ = 0;
    esp_err_t last_capacity_error_ = ESP_OK;
    std::function<void(bool)> on_charging_status_changed_;

    void CheckBatteryStatus();
    void LogBatterySample(uint8_t level);
};

#endif // ADC_BATTERY_MONITOR_H
