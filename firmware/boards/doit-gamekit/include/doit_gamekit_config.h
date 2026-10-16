#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace doit_gamekit {
inline constexpr char kName[] = "DOIT GameKit";
inline constexpr int kWidth = 320, kHeight = 240;
inline constexpr int kNativeWidth = kHeight, kNativeHeight = kWidth;
inline constexpr int kLcdBacklight = 7, kLcdReset = 8, kLcdDc = 9;
inline constexpr int kLcdCs = 10, kLcdMosi = 11, kLcdClock = 12;
inline constexpr int kAudioBclk = 4, kAudioWs = 5, kAudioDout = 6;
inline constexpr int kAudioSampleRate = 16000;
// V0.6 routes these signals differently from S31's fixed SDMMC IO MUX.
// Use SD SPI mode: CMD=MOSI, DAT0=MISO, DAT3=CS; DAT1/2 stay pulled up.
inline constexpr int kSdClock = 20, kSdMosi = 21, kSdMiso = 22;
inline constexpr int kSdD1 = 23, kSdD2 = 24, kSdCs = 25;
inline constexpr char kSdMount[] = "/sdcard"; // relative to CONFIG_PXA_MOUNT_POINT

enum Button : uint32_t {
    Up = 1u << 0, Down = 1u << 1, Left = 1u << 2, Right = 1u << 3,
    A = 1u << 4, B = 1u << 5, Start = 1u << 6, Select = 1u << 7,
    X = 1u << 8, Y = 1u << 9, L = 1u << 10, R = 1u << 11,
};
inline constexpr uint32_t kButtonMask = 0xfff;
// All switches connect to ground; configure internal pull-ups.
inline constexpr std::array<int, 12> kButtonPins = {
    46, 38, 40, 39, 16, 15, 18, 17, 1, 13, 47, 0,
};

constexpr bool PinsAreUnique() {
    constexpr std::array peripheral_pins = {
        kLcdBacklight, kLcdReset, kLcdDc, kLcdCs, kLcdMosi, kLcdClock,
        kAudioBclk, kAudioWs, kAudioDout, kSdClock, kSdMosi, kSdMiso,
        kSdD1, kSdD2, kSdCs,
    };
    for (size_t i = 0; i < kButtonPins.size(); ++i) {
        for (size_t j = 0; j < i; ++j)
            if (kButtonPins[i] == kButtonPins[j]) return false;
        for (int pin : peripheral_pins)
            if (kButtonPins[i] == pin) return false;
    }
    for (size_t i = 0; i < peripheral_pins.size(); ++i)
        for (size_t j = 0; j < i; ++j)
            if (peripheral_pins[i] == peripheral_pins[j]) return false;
    return true;
}
static_assert(PinsAreUnique());
}
