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

static lv_indev_data_t launcher_pointer;
static unsigned launcher_launches;
static void read_launcher_pointer(lv_indev_t* indev, lv_indev_data_t* data) {
    (void)indev;
    *data = launcher_pointer;
}
static void pointer_sample(lv_indev_t* indev, int x, int y, bool down) {
    launcher_pointer.point = (lv_point_t){x, y};
    launcher_pointer.state = down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    lv_tick_inc(20);
    lv_indev_read(indev);
}
static void* launcher_allocate(void* context, size_t size) {
    (void)context;
    return malloc(size);
}
static void launcher_release(void* context, void* memory) {
    (void)context;
    free(memory);
}
static pxsys_status_t launcher_launch_attempt(void* context,
    const pxsys_app_descriptor_t* app, uint64_t id, void** instance) {
    (void)context; (void)app; (void)id; (void)instance;
    ++launcher_launches;
    /* Count real task-manager launch attempts without replacing the test UI. */
    return PXSYS_STATUS_NO_MEMORY;
}
static void test_launcher_pointer(lv_display_t* display, unsigned dpi) {
    pxsys_standard_system_config_t config;
    pxsys_standard_system_config_init(&config);
    config.allocator.allocate = launcher_allocate;
    config.allocator.release = launcher_release;
    pxsys_reference_lvgl_t ui = {0};
    ui.magic = REFERENCE_MAGIC;
    assert(pxsys_standard_system_create(&config, &ui.system) == PXSYS_STATUS_OK);
    pxsys_display_profile_init(&ui.display, 296, 240);
    ui.display.density_dpi = dpi;
    pxsys_theme_snapshot_init(&ui.theme, PXSYS_COLOR_SCHEME_DARK);
    ui.root = lv_obj_create(lv_layer_top());
    style_plain(ui.root);
    lv_obj_set_size(ui.root, 296, 240);
    ui.content = lv_obj_create(ui.root);
    style_plain(ui.content);
    lv_obj_set_size(ui.content, 296, 240);
    lv_obj_set_scroll_dir(ui.content, LV_DIR_HOR);
    lv_obj_set_scroll_elastic(ui.content, false);
    launcher_item_t item = {.ui = &ui, .uninstallable = 1};
    strcpy(item.app_id, "test.launcher");
    item.identity.app_id = pxsys_string_from_cstr(item.app_id);
    memset(item.identity.publisher_root, 0x51, PXSYS_PUBLISHER_ROOT_BYTES);
    pxsys_app_descriptor_t app = {0};
    app.struct_size = sizeof(app);
    app.identity = item.identity;
    app.display_name = pxsys_string_from_cstr("Launcher test");
    app.version = pxsys_string_from_cstr("1.0.0");
    app.runtime_id = pxsys_string_from_cstr(PXSYS_NATIVE_RUNTIME_ID);
    app.flags = PXSYS_APP_FLAG_ENABLED;
    assert(pxsys_app_registry_register(pxsys_standard_system_apps(ui.system), &app) == PXSYS_STATUS_OK);
    pxsys_native_app_t implementation = {0};
    implementation.struct_size = sizeof(implementation);
    implementation.identity = item.identity;
    implementation.create = launcher_launch_attempt;
    implementation.start = app_start;
    implementation.foreground = app_foreground;
    implementation.background = app_background;
    implementation.event = app_event;
    implementation.back = app_back;
    implementation.stop = app_stop;
    implementation.destroy = app_destroy;
    assert(pxsys_native_runtime_register_app(pxsys_standard_system_native_runtime(ui.system), &implementation) == PXSYS_STATUS_OK);
    ui.launcher_items = &item;
    ui.launcher_count = 1;
    make_launcher_tile(&ui, "Test", &item);
    lv_obj_set_pos(item.tile, 25, 25);
    lv_obj_set_size(item.tile, 180, 150);
    lv_obj_update_layout(ui.root);
    lv_area_t tile_area, icon_area, label_area;
    lv_obj_get_coords(item.tile, &tile_area);
    lv_obj_get_coords(lv_obj_get_child(item.tile, 0), &icon_area);
    lv_obj_get_coords(lv_obj_get_child(item.tile, 1), &label_area);
    assert(LV_ABS(icon_area.y1 + label_area.y2 -
                  tile_area.y1 - tile_area.y2) <= 1);
    lv_indev_t* indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(indev, display);
    lv_indev_set_read_cb(indev, read_launcher_pointer);
    lv_timer_pause(lv_indev_get_read_timer(indev));
    const unsigned before = launcher_launches;
    /* A tap tolerates sensor jitter. */
    pointer_sample(indev, 80, 80, true);
    pointer_sample(indev, 83, 82, true);
    pointer_sample(indev, 83, 82, false);
    assert(launcher_launches == before + 1);
    /* One-page/outer-edge horizontal swipes cannot scroll, but must not launch. */
    pointer_sample(indev, 80, 80, true);
    pointer_sample(indev, 115, 80, true);
    pointer_sample(indev, 115, 80, false);
    assert(launcher_launches == before + 1);
    /* A slow vertical gesture returning to its origin stays cancelled. */
    pointer_sample(indev, 80, 80, true);
    for (int y = 82; y <= 110; y += 2) pointer_sample(indev, 80, y, true);
    pointer_sample(indev, 80, 80, true);
    pointer_sample(indev, 80, 80, false);
    assert(launcher_launches == before + 1);
    /* Movement that only appears in the release sample is also a swipe. */
    pointer_sample(indev, 80, 80, true);
    pointer_sample(indev, 115, 80, false);
    assert(launcher_launches == before + 1);
    /* The next intentional tap works after a cancelled gesture. */
    pointer_sample(indev, 80, 80, true);
    pointer_sample(indev, 80, 80, false);
    assert(launcher_launches == before + 2);
    /* Catching a page-settle animation must not open the moving icon. */
    lv_obj_t* end = lv_obj_create(ui.content);
    lv_obj_set_pos(end, 600, 0);
    lv_obj_set_size(end, 1, 1);
    lv_obj_update_layout(ui.root);
    lv_obj_scroll_to_x(ui.content, 100, LV_ANIM_ON);
    assert(lv_obj_is_scrolling(ui.content));
    pointer_sample(indev, 80, 80, true);
    pointer_sample(indev, 80, 80, false);
    assert(launcher_launches == before + 2);
    lv_obj_stop_scroll_anim(ui.content);
    lv_obj_scroll_to_x(ui.content, 0, LV_ANIM_OFF);
    /* A stationary long press still enters icon rearrangement. */
    pointer_sample(indev, 80, 80, true);
    lv_tick_inc(600);
    lv_indev_read(indev);
    assert(ui.launcher_editing);
    pointer_sample(indev, 80, 80, false);
    assert(launcher_launches == before + 2);
    /* The enlarged transparent margin is a real delete target. Swiping from
     * it must still cancel, including a return to the original point. */
    lv_obj_update_layout(ui.root);
    lv_area_t remove_area;
    lv_obj_get_coords(item.remove_button, &remove_area);
    assert(lv_area_get_width(&remove_area) == 40);
    const int rx = remove_area.x1 + 1, ry = remove_area.y1 + 20;
    pointer_sample(indev, rx, ry, true);
    pointer_sample(indev, rx, ry + 30, true);
    pointer_sample(indev, rx, ry, true);
    pointer_sample(indev, rx, ry, false);
    assert(ui.confirm_dialog == NULL);
    pointer_sample(indev, rx, ry, true);
    pointer_sample(indev, rx, ry, false);
    assert(ui.confirm_dialog != NULL);
    assert(ui.pending_action == PXSYS_REFERENCE_APP_ACTION_UNINSTALL);
    page_close_dialogs(&ui);
    lv_indev_delete(indev);
    lv_obj_delete(ui.root);
    assert(pxsys_standard_system_destroy(ui.system) == PXSYS_STATUS_OK);
}

static void settle_launcher(lv_indev_t* indev) {
    for (unsigned elapsed = 0; elapsed < 200; elapsed += 10) {
        lv_tick_inc(10);
        lv_indev_read(indev);
        (void)lv_timer_handler();
    }
}

static void test_launcher_paging(lv_display_t* display) {
    pxsys_reference_lvgl_t ui = {.magic = REFERENCE_MAGIC};
    pxsys_display_profile_init(&ui.display, 296, 240);
    pxsys_theme_snapshot_init(&ui.theme, PXSYS_COLOR_SCHEME_DARK);
    ui.root = lv_obj_create(lv_layer_top());
    style_plain(ui.root);
    lv_obj_set_size(ui.root, 296, 240);
    ui.content = lv_obj_create(ui.root);
    style_plain(ui.content);
    lv_obj_set_size(ui.content, 296, 200);
    lv_obj_set_y(ui.content, 25);
    pxsys_lvgl_add_flags(ui.content,
                        LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    launcher_configure_paging(&ui);
    launcher_item_t items[PXSYS_REFERENCE_UI_LAUNCHER_COLUMNS *
                          PXSYS_REFERENCE_UI_LAUNCHER_ROWS + 1] = {0};
    ui.launcher_items = items;
    ui.launcher_count = sizeof(items) / sizeof(items[0]);
    ui.launcher_gap = 4;
    for (unsigned i = 0; i < ui.launcher_count; ++i) {
        items[i].ui = &ui;
        make_launcher_tile(&ui, "Page test", &items[i]);
    }
    lv_obj_update_layout(ui.root);
    launcher_position_tiles(&ui);
    lv_obj_t* end = lv_obj_create(ui.content);
    style_plain(end);
    lv_obj_set_pos(end, 591, 0);
    lv_obj_set_size(end, 1, 1);
    lv_obj_update_layout(ui.root);
    lv_indev_t* indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(indev, display);
    lv_indev_set_read_cb(indev, read_launcher_pointer);
    lv_timer_pause(lv_indev_get_read_timer(indev));
    /* A normal drag settles directly to the next page within 200 ms. */
    pointer_sample(indev, 220, 80, true);
    for (int x = 200; x >= 100; x -= 20) pointer_sample(indev, x, 80, true);
    pointer_sample(indev, 100, 80, false);
    settle_launcher(indev);
    assert(ui.launcher_page == 1);
    assert(lv_obj_get_scroll_x(ui.content) == 296);
    assert(!lv_obj_is_scrolling(ui.content));
    /* A short fast flick goes back without requiring a long drag. */
    pointer_sample(indev, 80, 80, true);
    pointer_sample(indev, 100, 80, true);
    pointer_sample(indev, 120, 80, true);
    pointer_sample(indev, 120, 80, false);
    settle_launcher(indev);
    assert(ui.launcher_page == 0);
    assert(lv_obj_get_scroll_x(ui.content) == 0);
    /* A short drag with a pause at release returns to the current page. */
    pointer_sample(indev, 220, 80, true);
    pointer_sample(indev, 200, 80, true);
    pointer_sample(indev, 185, 80, true);
    for (unsigned i = 0; i < 12; ++i) pointer_sample(indev, 185, 80, true);
    pointer_sample(indev, 185, 80, false);
    settle_launcher(indev);
    assert(ui.launcher_page == 0);
    assert(lv_obj_get_scroll_x(ui.content) == 0);
    lv_indev_delete(indev);
    lv_obj_delete(ui.root);
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
    test_launcher_pointer(display, 160);
    test_launcher_pointer(display, 320);
    test_launcher_paging(display);
    lv_deinit();
    puts("Game gestures, launcher swipe rejection and compact shade status updates passed");
    return 0;
}
