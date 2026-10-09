/* Keep the real renderer while matching ESP32-S31 snapshot alignment. */
#include "../../../../deps/pxa-system/simulator/desktop/lv_conf.h"
#undef LV_DRAW_BUF_ALIGN
#define LV_DRAW_BUF_ALIGN 64
