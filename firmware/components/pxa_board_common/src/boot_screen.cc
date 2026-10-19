#include "pxa_board_common/boot_screen.h"

namespace pxa_board_common {

void BootScreen::Show(lv_display_t* display) {
    if (display == nullptr || content_ != nullptr) return;
    auto* screen = lv_display_get_screen_active(display);
    // Set the actual screen too: a missing label must never expose LVGL's
    // default white background during the rest of initialization.
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x111827), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    content_ = lv_obj_create(screen);
    if (content_ == nullptr) return;
    lv_obj_remove_style_all(content_);
    lv_obj_set_size(content_, lv_pct(100), lv_pct(100));
    lv_obj_set_pos(content_, 0, 0);
    lv_obj_remove_flag(content_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(content_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(content_, lv_color_hex(0x111827), 0);
    lv_obj_set_style_bg_opa(content_, LV_OPA_COVER, 0);
    auto* label = lv_label_create(content_);
    if (label == nullptr) return;
#if LV_FONT_MONTSERRAT_24
    lv_obj_set_style_text_font(label, &lv_font_montserrat_24, 0);
#elif LV_FONT_MONTSERRAT_20
    lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
#else
    lv_obj_set_style_text_font(label, LV_FONT_DEFAULT, 0);
#endif
    lv_obj_set_style_text_color(label, lv_color_hex(0xe5e7eb), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(label, "PXA\nStarting...");
    lv_obj_center(label);
}

void BootScreen::Hide() {
    if (content_ == nullptr) return;
    lv_obj_delete(content_);
    content_ = nullptr;
}

}  // namespace pxa_board_common
