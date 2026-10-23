#include "epd_display.h"

const EpdDisplay_t ED060XC3 = {
    .width = 1216,
    .height = 684,
    .bus_width = 16,
    .bus_speed = 24,
    .default_waveform = &epdiy_ED047TC2,
    .display_type = DISPLAY_TYPE_GENERIC,
};
