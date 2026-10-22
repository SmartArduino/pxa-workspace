#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace doit_gamekit_1 {
inline constexpr char kName[] = "DOIT GameKit 1";
inline constexpr int kWidth = 320, kHeight = 240;
// Match the physical scan direction; rotate logical landscape frames in software.
inline constexpr int kNativeWidth = kHeight, kNativeHeight = kWidth;
inline constexpr int kTouchNativeWidth = 240, kTouchNativeHeight = 320;
inline constexpr int kLcdBacklight = 7, kLcdReset = 8, kLcdDc = 9;
inline constexpr int kLcdCs = 10, kLcdMosi = 11, kLcdClock = 12;
inline constexpr int kAudioBclk = 4, kAudioWs = 5, kAudioDout = 6;
inline constexpr int kAudioSampleRate = 48000;
// V0.8 matches ESP32-S31 SDMMC slot 0's fixed IO MUX (four-bit mode).
inline constexpr int kSdClock = 24, kSdCmd = 25, kSdD0 = 20;
inline constexpr int kSdD1 = 21, kSdD2 = 22, kSdD3 = 23;
inline constexpr char kSdMount[] = "/sdcard"; // relative to CONFIG_PXA_MOUNT_POINT
// CL32CK305-18A_CTP: FT6336U, seven-bit I2C address 0x38.
inline constexpr int kTouchSda = 13, kTouchScl = 14;
inline constexpr int kTouchInterrupt = 15, kTouchReset = 16;
inline constexpr int kSystemI2cSda = 34, kSystemI2cScl = 33;
inline constexpr int kStatusLed = 47; // active low
inline constexpr int kBatteryAdc = 49; // BAT+ through R37/R52, 100k/100k

enum Button : uint32_t {
    Up = 1u << 0, Down = 1u << 1, Left = 1u << 2, Right = 1u << 3,
    A = 1u << 4, B = 1u << 5, Start = 1u << 6, Select = 1u << 7,
    X = 1u << 8, Y = 1u << 9, L = 1u << 10, R = 1u << 11,
    Menu = 1u << 12, Return = 1u << 13,
};
inline constexpr uint32_t kControllerMask = 0xfff;
inline constexpr uint32_t kButtonMask = kControllerMask | Menu | Return;
// All switches connect to ground; configure internal pull-ups.
inline constexpr std::array<int, 14> kButtonPins = {
    45, 42, 44, 43, 2, 17, 39, 40, 1, 3, 46, 0, 19, 18,
};

constexpr bool PinsAreUnique() {
    constexpr std::array peripheral_pins = {
        kLcdBacklight, kLcdReset, kLcdDc, kLcdCs, kLcdMosi, kLcdClock,
        kAudioBclk, kAudioWs, kAudioDout, kSdClock, kSdCmd, kSdD0,
        kSdD1, kSdD2, kSdD3, kTouchSda, kTouchScl, kTouchInterrupt,
        kTouchReset, kSystemI2cSda, kSystemI2cScl, kStatusLed, kBatteryAdc,
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
