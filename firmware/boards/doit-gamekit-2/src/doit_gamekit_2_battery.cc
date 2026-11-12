#include "doit_gamekit_2_battery.h"
#include "doit_gamekit_2_config.h"

#include <esp_adc/adc_cali_scheme.h>
#include <esp_log.h>
#include <driver/gpio.h>

namespace {
constexpr char kTag[] = "Gamekit2Battery";
constexpr int kSamples = 16;
// Keep margin below S31's approximately 2 V positive single-ended limit.
constexpr int kAdcLimitMv = 1980;
constexpr uint32_t kGraceMs = 10000;
constexpr uint32_t kUsbValid = 1u << 30, kUsbConnected = 1u << 29;
constexpr uint32_t kUsbMask = kUsbValid | kUsbConnected;

uint8_t VoltageToPercent(int mv) {
    // Approximate 4.2 V Li-ion discharge curve. Load, temperature and cell
    // chemistry affect this estimate. USB_POWER detects supply presence,
    // not charge current, battery presence or charge completion.
    constexpr struct { int mv; int percent; } curve[] = {
        {3300, 0}, {3500, 5}, {3600, 10}, {3700, 20}, {3750, 30},
        {3800, 45}, {3850, 55}, {3900, 65}, {4000, 80}, {4100, 90}, {4200, 100},
    };
    if (mv <= curve[0].mv) return 0;
    for (size_t i = 1; i < sizeof(curve) / sizeof(curve[0]); ++i) {
        if (mv <= curve[i].mv) {
            const auto& a = curve[i - 1];
            const auto& b = curve[i];
            return uint8_t(a.percent + ((mv - a.mv) * (b.percent - a.percent) +
                (b.mv - a.mv) / 2) / (b.mv - a.mv));
        }
    }
    return 100;
}
}

DoitGamekit2Battery::~DoitGamekit2Battery() { Release(); }

void DoitGamekit2Battery::Release() {
    if (calibration_) (void)adc_cali_delete_scheme_line_fitting(calibration_);
    if (adc_) (void)adc_oneshot_del_unit(adc_);
    calibration_ = nullptr;
    adc_ = nullptr;
    filtered_mv_ = 0;
    cached_.store(0, std::memory_order_release);
}

bool DoitGamekit2Battery::Initialize(uint32_t upper_kohm, uint32_t lower_kohm,
                                    uint32_t gain_permille, int32_t offset_mv) {
    Release();
    usb_seen_ = false;
    gpio_config_t usb_config{};
    usb_config.pin_bit_mask = UINT64_C(1) << doit_gamekit_2::kUsbPower;
    usb_config.mode = GPIO_MODE_INPUT;
    // Q4/R51 already pull the signal to 3.3 V. Never drive this input.
    usb_ready_ = gpio_config(&usb_config) == ESP_OK;
    if (!usb_ready_) ESP_LOGW(kTag, "USB power detection unavailable");
    sample_failed_ = out_of_range_ = false;
    if (!upper_kohm || !lower_kohm || upper_kohm > 1000 || lower_kohm > 1000 ||
        gain_permille < 500 || gain_permille > 1500 || offset_mv < -1000 || offset_mv > 1000)
        return false;
    upper_kohm_ = upper_kohm;
    lower_kohm_ = lower_kohm;
    gain_permille_ = gain_permille;
    offset_mv_ = offset_mv;
    adc_unit_t unit;
    esp_err_t error = adc_oneshot_io_to_channel(doit_gamekit_2::kBatteryAdc, &unit, &channel_);
    if (error == ESP_OK) {
        const adc_oneshot_unit_init_cfg_t config = {.unit_id = unit};
        error = adc_oneshot_new_unit(&config, &adc_);
    }
    if (error == ESP_OK) {
        const adc_oneshot_chan_cfg_t config = {
            .atten = ADC_ATTEN_DB_0, .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        error = adc_oneshot_config_channel(adc_, channel_, &config);
    }
    if (error == ESP_OK) {
        const adc_cali_line_fitting_config_t config = {
            .unit_id = unit, .atten = ADC_ATTEN_DB_0, .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        error = adc_cali_create_scheme_line_fitting(&config, &calibration_);
    }
    if (error != ESP_OK) {
        ESP_LOGW(kTag, "Battery ADC unavailable: %s", esp_err_to_name(error));
        Release();
        return false;
    }
    ESP_LOGI(kTag, "GPIO%d ADC%d channel %d, divider %lu/%lu kohm",
             doit_gamekit_2::kBatteryAdc, int(unit) + 1, int(channel_),
             static_cast<unsigned long>(upper_kohm_), static_cast<unsigned long>(lower_kohm_));
    return true;
}

void DoitGamekit2Battery::PollPower(uint32_t now_ms) {
    if (!usb_ready_) return;
    const bool connected = gpio_get_level(gpio_num_t(doit_gamekit_2::kUsbPower)) == 0;
    if (!usb_seen_ || connected != usb_candidate_) {
        usb_seen_ = true;
        usb_candidate_ = connected;
        usb_changed_ms_ = now_ms;
        return;
    }
    if (now_ms - usb_changed_ms_ < 50) return;
    const uint32_t previous = cached_.load(std::memory_order_acquire);
    const uint32_t power = kUsbValid | (connected ? kUsbConnected : 0);
    cached_.store((previous & ~kUsbMask) | power, std::memory_order_release);
    if ((previous & kUsbMask) != power)
        ESP_LOGI(kTag, "USB external power: %s (GPIO%d active low)",
                 connected ? "connected" : "disconnected", doit_gamekit_2::kUsbPower);
}

void DoitGamekit2Battery::Poll(uint32_t now_ms) {
    if (!adc_ || !calibration_) return;
    int total_mv = 0;
    bool saturated = false;
    esp_err_t error = ESP_OK;
    // Discard the first conversion to let the high-impedance divider settle.
    for (int i = 0; i <= kSamples; ++i) {
        int raw = 0, mv = 0;
        error = adc_oneshot_read(adc_, channel_, &raw);
        if (error == ESP_OK)
            error = adc_cali_chan_raw_to_voltage(calibration_, channel_, raw, &mv);
        if (error != ESP_OK) break;
        if (i == 0) continue;
        // Check each sample before averaging/filtering can conceal saturation.
        // Also reject a full-scale comparator vector if calibration shifts
        // the rail below the nominal voltage limit.
        saturated |= raw == 0x1ffff || mv >= kAdcLimitMv;
        total_mv += mv;
    }
    if (error != ESP_OK) {
        if (!sample_failed_) ESP_LOGW(kTag, "Battery sample failed: %s", esp_err_to_name(error));
        sample_failed_ = true;
        if (now_ms - last_valid_ms_ >= kGraceMs) {
            cached_.fetch_and(kUsbMask, std::memory_order_release);
            filtered_mv_ = 0;
        }
        return;
    }
    sample_failed_ = false;
    const int adc_mv = (total_mv + kSamples / 2) / kSamples;
    // Apply the supplied V2.0 board measurement correction after restoring
    // the divider voltage, retaining precision until the final rounding.
    const int64_t denominator = int64_t(lower_kohm_) * 1000;
    const int mv = int((int64_t(adc_mv) * (upper_kohm_ + lower_kohm_) * gain_permille_ +
                        denominator / 2) / denominator) + offset_mv_;
    // An absent cell and a saturated ADC cannot provide a usable estimate.
    if (saturated || mv < 2500 || mv > 4350) {
        if (!out_of_range_)
            ESP_LOGW(kTag, "Battery reading unusable: ADC=%d mV, battery=%d mV%s",
                     adc_mv, mv, saturated ? " (near ADC limit; check divider)" : "");
        out_of_range_ = true;
        cached_.fetch_and(kUsbMask, std::memory_order_release);
        filtered_mv_ = 0;
        return;
    }
    out_of_range_ = false;
    const auto previous = Get();
    if (!previous.valid || now_ms - last_valid_ms_ >= kGraceMs) filtered_mv_ = mv;
    else filtered_mv_ = (3 * filtered_mv_ + mv + 2) / 4;
    const uint8_t percent = VoltageToPercent(filtered_mv_);
    const uint32_t power = cached_.load(std::memory_order_acquire) & kUsbMask;
    cached_.store(power | 0x80000000u | (uint32_t(filtered_mv_) << 8) | percent,
                  std::memory_order_release);
    last_valid_ms_ = now_ms;
    if (!previous.valid || previous.percent != percent)
        ESP_LOGI(kTag, "Battery: ADC=%d mV, battery=%d mV, estimated=%u%%",
                 adc_mv, filtered_mv_, unsigned(percent));
}

DoitGamekit2Battery::Snapshot DoitGamekit2Battery::Get() const {
    const uint32_t value = cached_.load(std::memory_order_acquire);
    return {bool(value & 0x80000000u), uint8_t(value), uint16_t((value >> 8) & 0xffffu),
            bool(value & kUsbValid), bool(value & kUsbConnected)};
}
