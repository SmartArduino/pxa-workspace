#pragma once

#include <pxa_board_api.h>

/* Each board component exports exactly one registration entry point; `main`
 * links the one named by PXA_BOARD_COMPONENT. Using this macro keeps the
 * symbol name and its C linkage identical across boards. */
#define PXA_BOARD_REGISTER_SELECTED(REGISTER_FN)        \
    extern "C" bool pxa_board_register_selected(void) { \
        return REGISTER_FN();                           \
    }
