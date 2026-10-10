/* Exercise the real reference UI and LVGL renderer, including private gesture
 * helpers. The simulator's reference archive is not extracted when this
 * translation unit already supplies its public symbols. */
#include "../../../../deps/pxa-system/ui/reference/lvgl/src/reference_lvgl.c"
#include <assert.h>
#include <stdio.h>

static unsigned captures, hides, restores;
static bool fail_capture;
static uint16_t rendered_pixels[480 * 480];
static lv_draw_buf_t* capture_game(void* context, lv_obj_t* app) {
    assert(context == &captures);
    ++captures;
    if (fail_capture) return NULL;
    lv_draw_buf_t* image = lv_draw_buf_create(lv_obj_get_width(app),
        lv_obj_get_height(app), LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO);
    assert(image != NULL);
    for (unsigned y = 0; y < image->header.h; ++y) {
        uint16_t* row = lv_draw_buf_goto_xy(image, 0, y);
        for (unsigned x = 0; x < image->header.w; ++x)
            row[x] = ((x / 24 + y / 24) & 1) ? 0x07e0 : 0xf800;
    }
    return image;
}
static void presentation(void* context, bool visible) {
    assert(context == &captures);
    if (visible) ++restores; else ++hides;
}
static void flush(lv_display_t* display, const lv_area_t* area, uint8_t* pixels) {
    const lv_draw_buf_t* buffer = lv_display_get_buf_active(display);
    for (int32_t y = area->y1; y <= area->y2; ++y)
        memcpy(rendered_pixels + y * 480 + area->x1,
            pixels + (y - area->y1) * buffer->header.stride,
            (area->x2 - area->x1 + 1) * sizeof(uint16_t));
    lv_display_flush_ready(display);
}
static void advance(unsigned milliseconds) {
    for (unsigned i = 0; i < milliseconds; i += 10) {
        lv_tick_inc(10);
        (void)lv_timer_handler();
    }
}
int main(void) {
    lv_init();
    lv_display_t* display = lv_display_create(480, 480);
    static uint8_t pixels[480 * 16 * 2];
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, pixels, NULL, sizeof(pixels), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    pxsys_reference_lvgl_t ui = {0};
    ui.magic = REFERENCE_MAGIC;
    ui.parent = lv_layer_top();
    ui.root = lv_obj_create(ui.parent);
    style_plain(ui.root);
    lv_obj_set_size(ui.root, 480, 480);
    ui.display.width = ui.display.height = 480;
    ui.animations_enabled = 1;
    ui.application_presentation_context = &captures;
    ui.capture_application = capture_game;
    ui.application_presentation_changed = presentation;
    pxsys_theme_snapshot_init(&ui.theme, PXSYS_COLOR_SCHEME_DARK);

    lv_obj_t* app = application_root(&ui);
    lv_obj_set_style_bg_opa(app, LV_OPA_COVER, 0);
    application_motion_set(&ui, 224, 12, -24, 14);
    assert(captures == 1 && hides == 1 && restores == 0);
    assert(ui.application_motion_image != NULL && ui.application_motion_snapshot != NULL);
    assert(lv_obj_get_parent(ui.application_motion_image) == app);
    lv_obj_update_layout(app);
    lv_area_t transformed;
    lv_obj_get_coords(app, &transformed);
    lv_obj_get_transformed_area(app, &transformed, LV_OBJ_POINT_TRANSFORM_FLAG_NONE);
    assert(lv_area_get_width(&transformed) < 480 && lv_area_get_height(&transformed) < 480);
    assert(transformed.y1 != 0);
    application_motion_set(&ui, 208, 15, -31, 16);
    advance(50);
    unsigned game_pixels = 0;
    for (unsigned i = 0; i < 480 * 480; ++i)
        game_pixels += rendered_pixels[i] == 0x07e0 || rendered_pixels[i] == 0xf800;
    assert(game_pixels > 480 * 480 / 2 && game_pixels < 480 * 480 * 9 / 10);
    assert(rendered_pixels[0] != 0x07e0 && rendered_pixels[0] != 0xf800);
    const char* artifact = getenv("PXA_GESTURE_CAPTURE");
    if (artifact != NULL) {
        FILE* output = fopen(artifact, "wb");
        assert(output != NULL);
        fprintf(output, "P6\n480 480\n255\n");
        for (unsigned i = 0; i < 480 * 480; ++i) {
            const uint16_t c = rendered_pixels[i];
            const uint8_t rgb[] = {(uint8_t)((c >> 11) * 255 / 31),
                (uint8_t)(((c >> 5) & 63) * 255 / 63), (uint8_t)((c & 31) * 255 / 31)};
            fwrite(rgb, 1, 3, output);
        }
        fclose(output);
    }
    assert(captures == 1); /* Do not copy a full game frame on every drag sample. */

    pxsys_rect_t bounds = {0};
    lv_draw_buf_t* recent = capture_transformed_application(&ui, &bounds);
    assert(recent != NULL && bounds.width < 480 && bounds.height < 480);
    const uint16_t center = *(const uint16_t*)lv_draw_buf_goto_xy(
        recent, recent->header.w / 2, recent->header.h / 2);
    assert(center == 0x07e0 || center == 0xf800);
    assert(captures == 1 && ui.application_motion_image != NULL);
    lv_draw_buf_destroy(recent);

    /* Cancelling animates the existing snapshot back, then releases ownership. */
    animate_application(&ui, 256, 0, 0, NULL);
    advance(300);
    assert(ui.application_motion_image == NULL && ui.application_motion_snapshot == NULL);
    assert(restores == 1 && !ui.application_presentation_hidden);
    assert(lv_obj_get_style_transform_scale_x(app, 0) == 256);
    assert(application_x_get(app) == 0 && application_y_get(app) == 0);

    application_motion_set(&ui, 220, 9, -27, 14);
    assert(captures == 2 && hides == 2);
    ui.task_switcher = lv_obj_create(ui.parent);
    application_presentation_update(&ui);
    application_scale_reset(&ui);
    assert(ui.application_motion_image == NULL);
    assert(ui.application_presentation_hidden && restores == 1);
    close_task_switcher(&ui);
    assert(!ui.application_presentation_hidden && restores == 2);

    /* Rebuild/deletion must not leave the Surface permanently suppressed. */
    application_motion_set(&ui, 230, 0, -12, 8);
    lv_obj_delete(ui.application_motion_image);
    assert(ui.application_motion_snapshot == NULL && restores == 3);
    application_scale_reset(&ui);
    fail_capture = true;
    application_motion_set(&ui, 220, 0, -30, 14);
    const unsigned failed_count = captures;
    application_motion_set(&ui, 210, 0, -40, 18);
    assert(captures == failed_count && !ui.application_presentation_hidden);
    application_scale_reset(&ui);
    fail_capture = false;

    /* The very first visible Back sample must include the arrow, not a blank box. */
    assert(!navigation_back_indicator_update(&ui, 12, 12, 16, 240));
    lv_obj_t* indicator = ui.navigation_back_indicator;
    assert(indicator != NULL && !lv_obj_is_hidden(indicator));
    assert(!lv_obj_is_hidden(lv_obj_get_child(indicator, 0)));
    assert(lv_obj_get_style_x(indicator, 0) >= 12);
    assert(lv_obj_get_style_radius(indicator, 0) >= 14);
    lv_draw_buf_t* first = lv_snapshot_take(indicator, LV_COLOR_FORMAT_ARGB8888);
    lv_draw_buf_t* second = lv_snapshot_take(indicator, LV_COLOR_FORMAT_ARGB8888);
    assert(first != NULL && second != NULL);
    assert(first->header.w == second->header.w && first->header.h == second->header.h);
    assert(memcmp(first->data, second->data, first->header.stride * first->header.h) == 0);
    assert(((const lv_color32_t*)first->data)[0].alpha == 0);
    lv_draw_buf_destroy(first);
    lv_draw_buf_destroy(second);
    navigation_back_indicator_reset(&ui);
    assert(ui.navigation_back_indicator == NULL);

    /* Status samples must preserve the launcher and an open shade. Test the
     * real compact layout, including updates deferred until touch release. */
    pxsys_display_profile_init(&ui.display, 296, 240);
    ui.content_active = 1;
    ui.active_page = REFERENCE_PAGE_HOME;
    ui.text_font = LV_FONT_DEFAULT;
    pxsys_system_status_snapshot_init(&ui.system_status);
    ui.system_status.time_valid = ui.system_status.date_valid = 1;
    ui.system_status.hour = 12;
    ui.system_status.minute = 34;
    ui.system_status.year = 2026;
    ui.system_status.month = 10;
    ui.system_status.day = 10;
    ui.system_status.volume_supported = 1;
    ui.system_status.brightness_supported = 1;
    ui.system_status.volume_percent = 40;
    ui.system_status.brightness_percent = 70;
    lv_obj_t* launcher_sentinel = lv_obj_create(ui.root);
    build_notification_shade(&ui, 256);
    lv_obj_t* shade = ui.notification_shade;
    lv_obj_t* shade_content = ui.notification_content;
    lv_obj_t* clock = ui.notification_time;
    assert(clock != NULL && ui.notification_volume != NULL);
    lv_timer_t* status_timer = lv_timer_create(status_refresh_poll, 50, &ui);
    ui.system_status.minute = 35;
    ui.system_status.day = 11;
    ui.system_status.volume_percent = 55;
    ui.system_status.brightness_percent = 85;
    ui.status_refresh_pending = 1;
    ui.notification_dragging = 1;
    status_refresh_poll(status_timer);
    assert(ui.status_refresh_pending && strcmp(lv_label_get_text(clock), "12:34") == 0);
    ui.notification_dragging = 0;
    status_refresh_poll(status_timer);
    assert(!ui.status_refresh_pending);
    assert(ui.notification_shade == shade && ui.notification_content == shade_content);
    assert(ui.notification_time == clock && lv_obj_is_valid(launcher_sentinel));
    assert(strcmp(lv_label_get_text(clock), "12:35") == 0);
    assert(strcmp(lv_label_get_text(ui.notification_date), "2026-10-11") == 0);
    assert(lv_slider_get_value(ui.notification_volume) == 55);
    assert(lv_slider_get_value(ui.notification_brightness) == 85);
    /* A new supported control needs a new shade, but not new launcher icons. */
    ui.system_status.wifi_supported = 1;
    ui.system_status.wifi_enabled = 1;
    ui.status_refresh_pending = ui.status_rebuild_pending = 1;
    status_refresh_poll(status_timer);
    assert(lv_obj_is_valid(launcher_sentinel));
    assert(!ui.status_rebuild_pending && ui.notification_shade_open);
    assert(strcmp(lv_label_get_text(ui.notification_time), "12:35") == 0);
    close_notification_shade(&ui);
    ui.status_refresh_pending = 1;
    status_refresh_poll(status_timer);
    assert(lv_obj_is_valid(launcher_sentinel));
    lv_timer_delete(status_timer);

    lv_obj_delete(ui.task_switcher);
    lv_obj_delete(ui.root);
    lv_deinit();
    puts("Game gestures and compact shade status updates passed");
    return 0;
}
