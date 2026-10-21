#include "test_port.h"
#include <pxsys/lvgl_renderer.h>
#include "../../../../deps/pxa-system/ui/reference/lvgl/src/reference_lvgl.c"

static void* allocate(void* context, size_t size) {
    (void)context;
    return malloc(size);
}

static void release(void* context, void* memory) {
    (void)context;
    free(memory);
}

static void flush(lv_display_t* display, const lv_area_t* area, uint8_t* pixels) {
    (void)area;
    (void)pixels;
    lv_display_flush_ready(display);
}

static bool contains_label(lv_obj_t* object, const char* text) {
    if (lv_obj_has_class(object, &lv_label_class) &&
        strcmp(lv_label_get_text(object), text) == 0) return true;
    for (unsigned i = 0; i < lv_obj_get_child_count(object); ++i)
        if (contains_label(lv_obj_get_child(object, i), text)) return true;
    return false;
}

static void publish(pxsys_standard_system_t* system) {
    pxsys_system_status_snapshot_t snapshot = {.struct_size = sizeof(snapshot)};
    assert(pxsys_system_status_service_get(pxsys_standard_system_status(system),
                                           &snapshot) == PXSYS_STATUS_OK);
    assert(pxa_system_clock_update_status(&snapshot));
    assert(pxsys_system_status_service_update(pxsys_standard_system_status(system),
                                              &snapshot) == PXSYS_STATUS_OK);
}

int main(void) {
    reset();
    assert(pxa_system_clock_initialize());
    lv_init();
    lv_display_t* display = lv_display_create(480, 480);
    static uint8_t pixels[480 * 16 * 2];
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, pixels, NULL, sizeof(pixels), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    pxsys_allocator_t allocator = {
        .struct_size = sizeof(allocator), .allocate = allocate, .release = release,
    };
    pxsys_lvgl_renderer_config_t renderer_config;
    pxsys_lvgl_renderer_config_init(&renderer_config);
    renderer_config.allocator = allocator;
    renderer_config.parent = lv_screen_active();
    pxsys_lvgl_renderer_t* renderer;
    pxsys_renderer_provider_t provider;
    assert(pxsys_lvgl_renderer_create(&renderer_config, &renderer) == PXSYS_STATUS_OK);
    assert(pxsys_lvgl_renderer_provider(renderer, &provider) == PXSYS_STATUS_OK);
    pxsys_standard_system_config_t config;
    pxsys_standard_system_config_init(&config);
    config.allocator = allocator;
    config.initial_renderer = &provider;
    pxsys_display_profile_init(&config.initial_display, 480, 480);
    assert(pxsys_locale_snapshot_init(&config.initial_locale,
                                      pxsys_string_from_cstr("zh-CN")) == PXSYS_STATUS_OK);
    assert(pxa_system_clock_update_status(&config.initial_system_status));
    pxsys_standard_system_t* system;
    assert(pxsys_standard_system_create(&config, &system) == PXSYS_STATUS_OK);
    pxsys_reference_lvgl_config_t ui_config;
    pxsys_reference_lvgl_config_init(&ui_config);
    ui_config.system = system;
    ui_config.allocator = allocator;
    ui_config.parent = lv_layer_top();
    ui_config.animations_enabled = 0;
    pxsys_reference_lvgl_t* ui;
    assert(pxsys_reference_lvgl_create(&ui_config, &ui) == PXSYS_STATUS_OK);
    assert(pxsys_reference_lvgl_start(ui) == PXSYS_STATUS_OK);
    assert(ui->status_bar != NULL && contains_label(ui->status_bar, "00:00"));
    assert(!contains_label(ui->root, "--:--"));

    build_notification_shade(ui, 256);
    assert(strcmp(lv_label_get_text(ui->notification_time), "00:00") == 0);
    assert(strcmp(lv_label_get_text(ui->notification_date), "2026-01-01") == 0);
    wall_time += 61;
    publish(system);
    assert(contains_label(ui->status_bar, "00:01"));
    assert(strcmp(lv_label_get_text(ui->notification_time), "00:01") == 0);
    close_notification_shade(ui);

    assert(pxsys_reference_lvgl_set_locked(ui, true) == PXSYS_STATUS_OK);
    assert(strcmp(lv_label_get_text(ui->lock_time), "00:01") == 0);
    assert(strcmp(lv_label_get_text(ui->lock_date), "2026-01-01") == 0);
    wall_time += 86400 - 61;
    publish(system);
    assert(strcmp(lv_label_get_text(ui->lock_time), "00:00") == 0);
    assert(strcmp(lv_label_get_text(ui->lock_date), "2026-01-02") == 0);
    wall_time += 12 * 3600 + 34 * 60; /* network/manual clock correction */
    publish(system);
    assert(strcmp(lv_label_get_text(ui->lock_time), "12:34") == 0);
    shutdown_handler();
    reboot();
    assert(pxa_system_clock_initialize());
    publish(system);
    assert(strcmp(lv_label_get_text(ui->lock_time), "12:34") == 0);
    wall_time += 60;
    publish(system);
    assert(strcmp(lv_label_get_text(ui->lock_time), "12:35") == 0);
    assert(strcmp(lv_label_get_text(ui->lock_date), "2026-01-02") == 0);

    assert(pxsys_reference_lvgl_destroy(ui) == PXSYS_STATUS_OK);
    assert(pxsys_standard_system_destroy(system) == PXSYS_STATUS_OK);
    assert(pxsys_lvgl_renderer_destroy(renderer) == PXSYS_STATUS_OK);
    lv_deinit();
    puts("System clock + real LVGL: initial status, ticking status/shade/lock, midnight and restart restore passed");
    return 0;
}
