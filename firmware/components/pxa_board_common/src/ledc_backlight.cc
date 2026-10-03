#include "pxa_board_common/ledc_backlight.h"

#include <algorithm>

namespace pxa_board_common {

void LedcBacklight::Configure(ledc_channel_t channel, uint32_t max_duty,
                              uint8_t initial_percent, ledc_mode_t speed_mode) {
    speed_mode_ = speed_mode;
    channel_ = channel;
    max_duty_ = max_duty == 0 ? 1023 : max_duty;
    brightness_.store(std::min<uint8_t>(initial_percent, 100));
}

void LedcBacklight::SetPercent(uint8_t percent) {
    brightness_.store(std::min<uint8_t>(percent, 100));
}

void LedcBacklight::SetIdleDim(bool enabled, uint8_t percent) {
    idle_dim_percent_.store(enabled ? std::min<uint8_t>(percent, 100) : 100);
}

void LedcBacklight::Apply(bool screen_enabled) const {
    const uint8_t effective =
        std::min(brightness_.load(), idle_dim_percent_.load());
    const uint32_t duty =
        screen_enabled
            ? static_cast<uint32_t>(effective) * max_duty_ / 100
            : 0;
    ledc_set_duty(speed_mode_, channel_, duty);
    ledc_update_duty(speed_mode_, channel_);
}

}  // namespace pxa_board_common
