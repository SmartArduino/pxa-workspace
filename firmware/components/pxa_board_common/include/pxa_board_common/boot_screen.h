#pragma once

#include <lvgl.h>

namespace pxa_board_common {

// Call with the board/LVGL locks held. Uses only built-in fonts and remains
// below the system layer, so no storage or Host service is needed to draw it.
class BootScreen {
public:
    void Show(lv_display_t* display);
    void Hide();

private:
    lv_obj_t* content_ = nullptr;
};

}  // namespace pxa_board_common
