#pragma once
#include <cstdint>

namespace mosaico_board {
// BMI270 configured for +/-4 g and +/-2000 degrees/s, signed 16-bit.
constexpr int32_t AccelerationMicro(int16_t raw) {
    return int64_t(raw) * 4 * 9806650 / 32768;
}
constexpr int32_t AngularVelocityMicro(int16_t raw) {
    return int64_t(raw) * 34906585 / 32768;
}
constexpr int32_t TemperatureMilli(int16_t raw) {
    return 23000 + int32_t(raw) * 1000 / 512;
}
// This pinned Bosch API already divides its fixed-point result by 16 and
// returns whole microtesla, unlike older drivers returning 1/16 microtesla.
constexpr int32_t MagneticNano(int16_t compensated) {
    return int32_t(compensated) * 1000;
}
}
