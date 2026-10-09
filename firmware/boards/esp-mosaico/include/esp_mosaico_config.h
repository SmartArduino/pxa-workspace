#pragma once

#include <cstdint>
#include <optional>

namespace mosaico_board {

struct HardwareConfig {
    const char* name;
    int display_reset;
    int display_clock;
    int i2c_data;
    int i2c_clock;
    int codec_power;
    int status_led;
};

constexpr std::optional<HardwareConfig> DecodeHardwareConfig(uint16_t version) {
    switch (version) {
        case 0x0100:
            return HardwareConfig{"ESP-Mosaico V1.0", 42, 44, 0, 1, 56, 3};
        case 0x0101:
            return HardwareConfig{"ESP-Mosaico V1.1", 44, 42, 56, 3, -1, -1};
        case 0x0102:
            return HardwareConfig{"ESP-Mosaico V1.2", 44, 42, 56, 3, -1, -1};
        default:
            return std::nullopt;
    }
}

inline constexpr uint16_t kWidth = 480;
inline constexpr uint16_t kHeight = 480;
// Two 16-row buffers retain the original 32-row total SRAM footprint.
inline constexpr uint16_t kTransferRows = 16;
// 480 MHz BBPLL / 10 is exact on ESP32-S31 and below CO5300's 50 MHz limit.
inline constexpr uint32_t kPixelClockHz = 48000000;
inline constexpr uint8_t kMinimumPanelBrightnessPercent = 10;

// UI/NVS brightness is logical (2..100). CO5300's lowest raw values look off;
// map the visible range linearly, including idle dimming, at the panel boundary.
constexpr uint8_t PanelBrightnessPercent(uint8_t logical_percent) {
    if (logical_percent == 0) return 0;
    if (logical_percent <= 2) return kMinimumPanelBrightnessPercent;
    if (logical_percent >= 100) return 100;
    return kMinimumPanelBrightnessPercent +
        ((logical_percent - 2) * (100 - kMinimumPanelBrightnessPercent) + 49) / 98;
}

inline constexpr int kDisplayChipSelect = 50;
inline constexpr int kDisplayData0 = 36;
inline constexpr int kDisplayData1 = 51;
inline constexpr int kDisplayData2 = 35;
inline constexpr int kDisplayData3 = 9;
inline constexpr int kDisplayTe = 43;
inline constexpr int kTouchInterrupt = 6;
inline constexpr int kFunctionButton = 7;
inline constexpr int kPeripheralPower = 60;
inline constexpr int kPowerSwitch = 57;
inline constexpr int kAudioBitClock = 37;
inline constexpr int kAudioWordSelect = 49;
inline constexpr int kAudioMasterClock = 54;
inline constexpr int kAudioDataOut = 52;
inline constexpr int kAudioAmplifierEnable = 45;
inline constexpr uint8_t kAudioCodecAddress = 0x19;
inline constexpr uint32_t kAudioSampleRate = 16000;

template <typename Area>
void RoundDisplayArea(Area* area) {
    if (area == nullptr) return;
    area->x1 = (area->x1 < 0 ? 0 : area->x1) & ~3;
    area->y1 = (area->y1 < 0 ? 0 : area->y1) & ~1;
    area->x2 = (area->x2 >= kWidth ? kWidth - 1 : area->x2) | 3;
    area->y2 = (area->y2 >= kHeight ? kHeight - 1 : area->y2) | 1;
}

template <typename Area>
bool ValidDisplayArea(const Area& area) {
    return area.x1 >= 0 && area.y1 >= 0 && area.x2 >= area.x1 &&
        area.y2 >= area.y1 && area.x2 < kWidth && area.y2 < kHeight;
}

}
