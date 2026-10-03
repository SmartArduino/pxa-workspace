#pragma once

#include <atomic>
#include <cstdint>

#include <driver/ledc.h>

namespace pxa_board_common {

/* LEDC panel backlight: the user's brightness, the idle-dim level and the duty
 * math that turns them into a duty cycle, gated by the screen state.
 *
 * Boards keep their own LEDC timer, GPIO and channel setup; this owns the
 * state and the policy both pai-touch and the SenseCAP Watcher used to repeat
 * (min(brightness, idle_dim), 0 while the screen is off). */
class LedcBacklight {
public:
    void Configure(ledc_channel_t channel, uint32_t max_duty = 1023,
                   uint8_t initial_percent = 75,
                   ledc_mode_t speed_mode = LEDC_LOW_SPEED_MODE);

    /* Clamped to 0..100. */
    void SetPercent(uint8_t percent);
    /* `enabled == false` restores full brightness. */
    void SetIdleDim(bool enabled, uint8_t percent);
    /* Applies the effective duty; call it after SetPercent/SetIdleDim and
     * whenever the screen is switched on or off. */
    void Apply(bool screen_enabled) const;

    uint8_t percent() const { return brightness_.load(); }
    uint8_t idle_dim_percent() const { return idle_dim_percent_.load(); }

private:
    ledc_mode_t speed_mode_ = LEDC_LOW_SPEED_MODE;
    ledc_channel_t channel_ = LEDC_CHANNEL_0;
    uint32_t max_duty_ = 1023;
    std::atomic<uint8_t> brightness_{75};
    std::atomic<uint8_t> idle_dim_percent_{100};
};

}  // namespace pxa_board_common
