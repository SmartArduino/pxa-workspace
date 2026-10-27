#include "pxa_board_common/boot_screen.h"
#include "assets/pxa_boot_logo.h"

namespace pxa_board_common {

static const lv_image_dsc_t kLogo = {
    .header = {
        .magic = LV_IMAGE_HEADER_MAGIC,
        .cf = LV_COLOR_FORMAT_RGB565,
        .flags = 0,
        .w = pxa_brand::kWidth,
        .h = pxa_brand::kHeight,
        .stride = pxa_brand::kWidth * sizeof(uint16_t),
        .reserved_2 = 0,
    },
    .data_size = sizeof(pxa_brand::kBootRgb565),
    .data = reinterpret_cast<const uint8_t*>(pxa_brand::kBootRgb565),
    .reserved = nullptr,
    .reserved_2 = nullptr,
};

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
    // Embedded pixels: no storage, font file or runtime image decoder needed.
    auto* logo = lv_image_create(content_);
    if (logo != nullptr) {
        lv_image_set_src(logo, &kLogo);
        lv_obj_set_size(logo, lv_pct(100), lv_pct(100));
        lv_image_set_inner_align(logo, LV_IMAGE_ALIGN_CONTAIN);
        lv_obj_center(logo);
    }
    auto* label = lv_label_create(content_);
    if (label == nullptr) return;
#if LV_FONT_MONTSERRAT_14
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
#else
    lv_obj_set_style_text_font(label, LV_FONT_DEFAULT, 0);
#endif
    lv_obj_set_style_text_color(label, lv_color_hex(0xe5e7eb), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(label, logo != nullptr ? "Starting..." : "PXA\nStarting...");
    lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0,
        -static_cast<int32_t>(lv_display_get_vertical_resolution(display) / 15));
}

void BootScreen::Hide() {
    if (content_ == nullptr) return;
    lv_obj_delete(content_);
    content_ = nullptr;
}

}  // namespace pxa_board_common
