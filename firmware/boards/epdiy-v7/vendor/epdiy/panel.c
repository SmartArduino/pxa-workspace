#include "epdiy.h"
#include "waveforms/epdiy_ED047TC2.h"

// ED060XC3 is repurposed by the supplied 40-pin reference firmware.
const EpdDisplay_t epdiy_v7_panel = {
    .width = 1216, .height = 684, .bus_width = 16, .bus_speed = 24,
    .default_waveform = &epdiy_ED047TC2, .display_type = DISPLAY_TYPE_GENERIC,
};
