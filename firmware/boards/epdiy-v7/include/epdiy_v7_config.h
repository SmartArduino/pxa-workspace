#pragma once
#include "sdkconfig.h"
namespace epdiy_v7 {
inline constexpr int kNativeWidth = 1216;
inline constexpr int kNativeHeight = 684;
#if CONFIG_PXA_EPDIY_PORTRAIT
inline constexpr int kWidth = kNativeHeight;
inline constexpr int kHeight = kNativeWidth;
inline constexpr bool kPortrait = true;
#else
inline constexpr int kWidth = kNativeWidth;
inline constexpr int kHeight = kNativeHeight;
inline constexpr bool kPortrait = false;
#endif
}
