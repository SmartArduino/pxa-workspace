#include "adc_battery_monitor.h"
#include "esp_log.h"

namespace {
constexpr int64_t kBatteryReadingGracePeriodUs = 5 * 1000 * 1000;
constexpr int64_t kBatteryDiagnosticLogIntervalUs = 10 * 1000 * 1000;
}

AdcBatteryMonitor::AdcBatteryMonitor(adc_unit_t adc_unit, adc_channel_t adc_channel,
                                     float upper_resistor, float lower_resistor,
                                     gpio_num_t charging_pin,
                                     adc_oneshot_unit_handle_t adc_handle)
    : charging_pin_(charging_pin),
      adc_handle_(adc_handle),
      adc_channel_(adc_channel),
      voltage_divider_gain_(lower_resistor > 0.0f
                                 ? (upper_resistor + lower_resistor) / lower_resistor
                                 : 0.0f) {
    
    // Initialize charging pin (only if it's not NC)
    if (charging_pin_ != GPIO_NUM_NC) {
        gpio_config_t gpio_cfg = {
            .pin_bit_mask = 1ULL << charging_pin,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&gpio_cfg));
    }

    adc_battery_estimation_t adc_cfg = {
        .adc_channel = adc_channel,
        .upper_resistor = upper_resistor,
        .lower_resistor = lower_resistor,
    };
    if (adc_handle != nullptr) {
        adc_oneshot_chan_cfg_t channel_cfg = {
            .atten = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, adc_channel,
                                                   &channel_cfg));

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
        adc_cali_curve_fitting_config_t cali_cfg = {
            .unit_id = adc_unit,
            .chan = adc_channel,
            .atten = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        ESP_ERROR_CHECK(adc_cali_create_scheme_curve_fitting(&cali_cfg,
                                                              &adc_cali_handle_));
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
        adc_cali_line_fitting_config_t cali_cfg = {
            .unit_id = adc_unit,
            .atten = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        ESP_ERROR_CHECK(adc_cali_create_scheme_line_fitting(&cali_cfg,
                                                             &adc_cali_handle_));
#endif
        adc_cfg.external.adc_handle = adc_handle;
        adc_cfg.external.adc_cali_handle = adc_cali_handle_;
        owns_adc_calibration_ = adc_cali_handle_ != nullptr;
    } else {
        adc_cfg.internal.adc_unit = adc_unit;
        adc_cfg.internal.adc_bitwidth = ADC_BITWIDTH_DEFAULT;
        adc_cfg.internal.adc_atten = ADC_ATTEN_DB_12;
    }

    // 在ADC配置部分进行条件设置
    if (charging_pin_ != GPIO_NUM_NC) {
        adc_cfg.charging_detect_cb = [](void *user_data) -> bool {
            AdcBatteryMonitor *self = (AdcBatteryMonitor *)user_data;
            return gpio_get_level(self->charging_pin_) == 1;
        };
        adc_cfg.charging_detect_user_data = this;
    } else {
        // 不设置回调，让adc_battery_estimation库使用软件估算
        adc_cfg.charging_detect_cb = nullptr;
        adc_cfg.charging_detect_user_data = nullptr;
    }
    adc_battery_estimation_handle_ = adc_battery_estimation_create(&adc_cfg);

    // Initialize timer
    esp_timer_create_args_t timer_cfg = {
        .callback = [](void *arg) {
            AdcBatteryMonitor *self = (AdcBatteryMonitor *)arg;
            self->CheckBatteryStatus();
        },
        .arg = this,
        .name = "adc_battery_monitor",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_cfg, &timer_handle_));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer_handle_, 1000000));
}

AdcBatteryMonitor::~AdcBatteryMonitor() {
    if (adc_battery_estimation_handle_) {
        ESP_ERROR_CHECK(adc_battery_estimation_destroy(adc_battery_estimation_handle_));
    }

    if (owns_adc_calibration_) {
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
        ESP_ERROR_CHECK(adc_cali_delete_scheme_curve_fitting(adc_cali_handle_));
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
        ESP_ERROR_CHECK(adc_cali_delete_scheme_line_fitting(adc_cali_handle_));
#endif
    }
    
    if (timer_handle_) {
        esp_timer_stop(timer_handle_);
        esp_timer_delete(timer_handle_);
    }
}

bool AdcBatteryMonitor::IsCharging() {
    // 优先使用adc_battery_estimation库的功能
    if (adc_battery_estimation_handle_ != nullptr) {
        bool is_charging = false;
        esp_err_t err = adc_battery_estimation_get_charging_state(adc_battery_estimation_handle_, &is_charging);
        if (err == ESP_OK) {
            return is_charging;
        }
    }
    
    // 回退到GPIO读取或返回默认值
    if (charging_pin_ != GPIO_NUM_NC) {
        return gpio_get_level(charging_pin_) == 1;
    }
    
    return false;
}

bool AdcBatteryMonitor::IsDischarging() {
    return !IsCharging();
}

bool AdcBatteryMonitor::GetBatteryLevel(uint8_t& level) {
    float capacity = 0;
    esp_err_t err = adc_battery_estimation_handle_ == nullptr
                        ? ESP_ERR_INVALID_STATE
                        : adc_battery_estimation_get_capacity(
                              adc_battery_estimation_handle_, &capacity);
    if (err == ESP_OK && !(capacity >= 0.0f && capacity <= 100.0f)) {
        err = ESP_ERR_INVALID_RESPONSE;
    }
    if (err == ESP_OK) {
        last_battery_level_ = static_cast<uint8_t>(capacity + 0.5f);
        has_valid_battery_level_ = true;
        last_battery_sample_time_us_ = esp_timer_get_time();
        last_capacity_error_ = ESP_OK;
        level = last_battery_level_;
        LogBatterySample(level);
        return true;
    }

    if (err != last_capacity_error_) {
        ESP_LOGW("AdcBatteryMonitor", "Battery capacity unavailable: %s",
                 esp_err_to_name(err));
        last_capacity_error_ = err;
    }

    // Preserve the last valid value only for a brief transient ADC failure.
    if (has_valid_battery_level_ &&
        esp_timer_get_time() - last_battery_sample_time_us_ <= kBatteryReadingGracePeriodUs) {
        level = last_battery_level_;
        return true;
    }
    return false;
}

uint8_t AdcBatteryMonitor::GetBatteryLevel() {
    uint8_t level = last_battery_level_;
    GetBatteryLevel(level);
    return level;
}

void AdcBatteryMonitor::OnChargingStatusChanged(std::function<void(bool)> callback) {
    on_charging_status_changed_ = callback;
}

void AdcBatteryMonitor::LogBatterySample(uint8_t level) {
    const int64_t now = esp_timer_get_time();
    if (adc_handle_ == nullptr || adc_cali_handle_ == nullptr ||
        (last_diagnostic_log_time_us_ != 0 &&
         now - last_diagnostic_log_time_us_ < kBatteryDiagnosticLogIntervalUs)) {
        return;
    }
    last_diagnostic_log_time_us_ = now;

    int raw = 0;
    int adc_mv = 0;
    const esp_err_t read_err = adc_oneshot_read(adc_handle_, adc_channel_, &raw);
    const esp_err_t calibration_err = read_err == ESP_OK
                                          ? adc_cali_raw_to_voltage(adc_cali_handle_, raw, &adc_mv)
                                          : read_err;
    if (calibration_err != ESP_OK) {
        ESP_LOGW("AdcBatteryMonitor", "Battery diagnostic sample failed: %s",
                 esp_err_to_name(calibration_err));
        return;
    }

    const int battery_mv = static_cast<int>(adc_mv * voltage_divider_gain_ + 0.5f);
    ESP_LOGI("AdcBatteryMonitor",
             "Battery: channel=%d raw=%d adc=%dmV battery=%dmV level=%u%% charging=%d",
             static_cast<int>(adc_channel_), raw, adc_mv, battery_mv,
             static_cast<unsigned>(level), IsCharging());
}

void AdcBatteryMonitor::CheckBatteryStatus() {
    bool new_charging_status = IsCharging();
    if (new_charging_status != is_charging_) {
        is_charging_ = new_charging_status;
        if (on_charging_status_changed_) {
            on_charging_status_changed_(is_charging_);
        }
    }
}
