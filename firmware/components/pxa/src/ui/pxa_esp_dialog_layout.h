#ifndef PXA_ESP_DIALOG_LAYOUT_H
#define PXA_ESP_DIALOG_LAYOUT_H

#include "lvgl.h"

#define PXA_ESP_DIALOG_MIN_WIDTH 224
#define PXA_ESP_DIALOG_MAX_WIDTH 400
#define PXA_ESP_DIALOG_MIN_HEIGHT 136
#define PXA_ESP_DIALOG_MAX_HEIGHT 360
#define PXA_ESP_DIALOG_FOOTER_HEIGHT 76

typedef struct {
    int32_t width;
    int32_t height;
    int32_t content_top;
    int32_t content_height;
    int32_t button_width;
} pxa_esp_dialog_layout_t;

static inline int32_t pxa_esp_dialog_limit(int32_t value, int32_t minimum,
                                            int32_t maximum) {
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

static inline lv_point_t pxa_esp_dialog_text_size(const char *text,
                                                  const lv_font_t *font,
                                                  int32_t max_width,
                                                  int32_t line_space) {
    lv_point_t size;
    lv_text_get_size(&size, text, font, 0, line_space, max_width,
                     LV_TEXT_FLAG_NONE);
    return size;
}

static inline pxa_esp_dialog_layout_t pxa_esp_dialog_measure(
    int32_t screen_width, int32_t screen_height, const char *title,
    const char *first, const char *second, const lv_font_t *heading_font,
    const lv_font_t *text_font, int32_t first_line_space,
    int32_t content_gap) {
    pxa_esp_dialog_layout_t layout;
    int32_t max_width = pxa_esp_dialog_limit(
        screen_width - 28, 1, PXA_ESP_DIALOG_MAX_WIDTH);
    int32_t max_height = pxa_esp_dialog_limit(
        screen_height - 24, 1, PXA_ESP_DIALOG_MAX_HEIGHT);
    int32_t text_width = pxa_esp_dialog_text_size(
        title, heading_font, LV_COORD_MAX, 0).x;
    int32_t first_width = pxa_esp_dialog_text_size(
        first, text_font, LV_COORD_MAX, first_line_space).x;
    if (first_width > text_width) text_width = first_width;
    if (second != NULL) {
        int32_t second_width = pxa_esp_dialog_text_size(
            second, text_font, LV_COORD_MAX, 0).x;
        if (second_width > text_width) text_width = second_width;
    }
    layout.width = pxa_esp_dialog_limit(text_width + 30,
                                        max_width < PXA_ESP_DIALOG_MIN_WIDTH ?
                                            max_width : PXA_ESP_DIALOG_MIN_WIDTH,
                                        max_width);
    int32_t inner_width = layout.width - 30;
    int32_t title_height = pxa_esp_dialog_text_size(
        title, heading_font, inner_width, 0).y;
    int32_t content_height = pxa_esp_dialog_text_size(
        first, text_font, inner_width, first_line_space).y;
    if (second != NULL) {
        content_height += content_gap + pxa_esp_dialog_text_size(
            second, text_font, inner_width, 0).y;
    }
    layout.content_top = title_height + 12;
    layout.height = pxa_esp_dialog_limit(
        layout.content_top + content_height + PXA_ESP_DIALOG_FOOTER_HEIGHT,
        max_height < PXA_ESP_DIALOG_MIN_HEIGHT ?
            max_height : PXA_ESP_DIALOG_MIN_HEIGHT, max_height);
    layout.content_height = layout.height - layout.content_top -
                            PXA_ESP_DIALOG_FOOTER_HEIGHT;
    if (layout.content_height < 1) layout.content_height = 1;
    layout.button_width = pxa_esp_dialog_limit((layout.width - 36) / 2, 1, 92);
    return layout;
}

#endif
