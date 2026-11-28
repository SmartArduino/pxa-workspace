/* Exercise the real Back affordance and ESP alpha provider with S31 buffer
 * alignment, including deliberately unfavourable PSRAM pointer alignment. */
#include "../../../../deps/pxa-system/ui/reference/lvgl/src/reference_lvgl.c"
#include <assert.h>
#include <stdio.h>

#define ESP_PLATFORM 1
#include "../../pxa/src/ui/pxa_esp_system_overlay.c"

static unsigned overlay_failures, overlay_allocations;
void test_enter(void) {}
void test_leave(void) {}
void test_log(const char* tag, const char* format, ...) {
    (void)tag;
    (void)format;
    ++overlay_failures;
}
/* Force the largest possible address-alignment loss. Ordinary desktop malloc
 * often leaves enough accidental padding to conceal the device-only defect. */
static void* overlay_allocate(size_t size, bool misaligned) {
    uint8_t* base = malloc(size + LV_DRAW_BUF_ALIGN + sizeof(void*));
    assert(base != NULL);
    uintptr_t start = ((uintptr_t)(base + sizeof(void*)) + LV_DRAW_BUF_ALIGN - 1u) /
        LV_DRAW_BUF_ALIGN * LV_DRAW_BUF_ALIGN + (misaligned ? 1u : 0u);
    memcpy((void*)(start - sizeof(void*)), &base, sizeof(base));
    ++overlay_allocations;
    return (void*)start;
}
void* heap_caps_malloc(size_t size, unsigned caps) {
    (void)caps;
    return overlay_allocate(size, true);
}
void* heap_caps_calloc(size_t count, size_t size, unsigned caps) {
    (void)caps;
    void* memory = overlay_allocate(count * size, false);
    memset(memory, 0, count * size);
    return memory;
}
void heap_caps_free(void* memory) {
    if (memory == NULL) return;
    void* base;
    memcpy(&base, (uint8_t*)memory - sizeof(void*), sizeof(base));
    free(base);
    --overlay_allocations;
}
bool pxa_esp_surface_has_visible_surface(void) { return true; }
void pxa_esp_surface_set_system_alpha_provider(
    pxa_esp_surface_ui_alpha_provider_fn provider, void* context) {
    (void)provider;
    (void)context;
}
void pxa_esp_surface_require_composition(void) {}
static void overlay_objects(void* context, lv_obj_t* const* objects, size_t count) {
    (void)context;
    pxa_esp_system_overlay_set_reference_objects(objects, count);
}

static void flush(lv_display_t* display, const lv_area_t* area, uint8_t* pixels) {
    (void)area;
    (void)pixels;
    lv_display_flush_ready(display);
}

static uint16_t overlay_pixel(int32_t x, int32_t y) {
    assert(g_plane.visible && g_plane.pixels != NULL && g_plane.alpha != NULL);
    assert(x >= g_plane.x && x < g_plane.x + g_plane.width);
    assert(y >= g_plane.y && y < g_plane.y + g_plane.height);
    const size_t index = (size_t)(y - g_plane.y) * g_plane.width + x - g_plane.x;
    assert(g_plane.alpha[index] != 0);
    return g_plane.pixels[index];
}

static void check_shade_levels(pxsys_reference_lvgl_t* ui, lv_display_t* display) {
    ui->notification_shade = lv_obj_create(ui->root);
    style_plain(ui->notification_shade);
    lv_obj_set_size(ui->notification_shade, 480, 480);
    ui->notification_viewport = lv_obj_create(ui->notification_shade);
    style_plain(ui->notification_viewport);
    lv_obj_set_size(ui->notification_viewport, 460, 200);
    ui->notification_volume = make_control_slider(ui, ui->notification_viewport,
        10, 20, 420, 40, "Volume", 25, PXSYS_LEVEL_CONTROL_VOLUME, &ui->shade_levels[0]);
    ui->notification_brightness = make_control_slider(ui, ui->notification_viewport,
        10, 90, 420, 40, "Brightness", 25, PXSYS_LEVEL_CONTROL_BRIGHTNESS, &ui->shade_levels[1]);
    ui->notification_shade_open = 1;
    ui->notification_progress = 256;
    ui->notification_refresh_hook = lv_display_add_event_cb(
        display, notification_refresh_event, LV_EVENT_REFR_START, ui) != NULL;
    assert(ui->notification_refresh_hook);
    update_system_overlay(ui);
    const uint16_t initial_volume = overlay_pixel(265, 49);
    const uint16_t initial_brightness = overlay_pixel(265, 119);
    const uint64_t initial_revision = g_plane.revision;

    /* Two touch samples arrive before LCD refresh. Publish the latest fill
     * and percentage to the game compositor in one plane update. */
    lv_slider_set_value(ui->notification_volume, 45, LV_ANIM_OFF);
    lv_obj_send_event(ui->notification_volume, LV_EVENT_VALUE_CHANGED, NULL);
    lv_slider_set_value(ui->notification_volume, 80, LV_ANIM_OFF);
    lv_obj_send_event(ui->notification_volume, LV_EVENT_VALUE_CHANGED, NULL);
    assert(strcmp(lv_label_get_text(ui->shade_levels[0].readout), "80%") == 0);
    assert(g_plane.revision == initial_revision);
    lv_refr_now(display);
    assert(g_plane.revision == initial_revision + 1);
    assert(overlay_pixel(265, 49) != initial_volume);
    assert(overlay_pixel(265, 119) == initial_brightness);

    ui->system_status.volume_percent = 80;
    ui->system_status.brightness_percent = 80;
    notification_refresh_values(ui);
    assert(strcmp(lv_label_get_text(ui->shade_levels[1].readout), "80%") == 0);
    lv_refr_now(display);
    assert(g_plane.revision == initial_revision + 2);
    assert(overlay_pixel(265, 119) != initial_brightness);

    /* Closing while a repaint is pending must drop both the plane and the
     * pending refresh. The next frame must not bring the drawer back. */
    lv_slider_set_value(ui->notification_volume, 70, LV_ANIM_OFF);
    lv_obj_send_event(ui->notification_volume, LV_EVENT_VALUE_CHANGED, NULL);
    close_notification_shade(ui);
    const uint64_t closed_revision = g_plane.revision;
    lv_refr_now(display);
    assert(!g_plane.visible && g_plane.revision == closed_revision);
    assert(overlay_allocations == 0);
    lv_display_remove_event_cb_with_user_data(display, notification_refresh_event, ui);
}

int main(void) {
    assert(LV_DRAW_BUF_ALIGN == 64);
    lv_init();
    lv_display_t* display = lv_display_create(480, 480);
    static uint8_t pixels[480 * 16 * 2];
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565_SWAPPED);
    lv_display_set_buffers(display, pixels, NULL, sizeof(pixels), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    pxsys_reference_lvgl_t ui = {0};
    ui.magic = REFERENCE_MAGIC;
    ui.parent = lv_layer_top();
    ui.root = lv_obj_create(ui.parent);
    style_plain(ui.root);
    lv_obj_set_size(ui.root, 480, 480);
    ui.display.width = ui.display.height = 480;
    ui.display.safe_insets.left = 12;
    ui.system_overlay_objects_changed = overlay_objects;
    pxsys_theme_snapshot_init(&ui.theme, PXSYS_COLOR_SCHEME_DARK);
    pxa_esp_system_overlay_bind();
    /* Use the actual ESP overlay snapshot/provider with the device's 64-byte
     * draw-buffer rules. Every Back progress size must preserve game pixels
     * outside the small alpha plane instead of publishing an opaque fallback. */
    for (int distance = 1; distance <= 100; ++distance) {
        navigation_back_indicator_update(&ui, 12, 12 + distance, 240);
        pxa_esp_surface_ui_alpha_plane_t plane;
        assert(provide_plane(NULL, &plane));
        assert(plane.visible && plane.pixels != NULL && plane.alpha != NULL);
        assert(plane.width < 64 && plane.height < 96);
        assert(plane.x == 0 && plane.y > 0);
        assert(plane.alpha[0] == 255);
        if (distance >= 12) assert(plane.alpha[plane.width - 1] == 0);
        assert(!ui.application_presentation_hidden);
    }
    assert(overlay_failures == 0);
    navigation_back_indicator_reset(&ui);
    assert(!g_plane.visible && overlay_allocations == 0);
    /* A temporarily revealed hidden status bar must enter the trusted overlay
     * object list. Its alpha plane is only a top strip, not an opaque game-sized
     * fallback; hidden navigation remains absent. */
    pxsys_display_profile_init(&ui.display, 480, 480);
    ui.display.density_dpi = 305;
    ui.display.safe_insets = (pxsys_insets_t){12, 12, 12, 12};
    ui.display.shape = PXSYS_DISPLAY_SHAPE_ROUNDED_RECTANGLE;
    ui.display.corner_radii = (pxsys_corner_radii_t){58, 58, 58, 58};
    ui.text_font = LV_FONT_DEFAULT;
    ui.window.status_bar_mode = PXSYS_WINDOW_BAR_HIDDEN;
    ui.window.navigation_bar_mode = PXSYS_WINDOW_BAR_HIDDEN;
    ui.transient_revealed = CHROME_REVEALED_HIDDEN_STATUS;
    pxsys_reference_layout_t layout;
    assert(pxsys_reference_layout_compute(&ui.display, &layout) == PXSYS_STATUS_OK);
    build_status_bar(&ui, &layout);
    update_system_overlay(&ui);
    pxa_esp_surface_ui_alpha_plane_t plane;
    assert(provide_plane(NULL, &plane));
    assert(plane.visible && plane.alpha != NULL && plane.pixels != NULL);
    assert(plane.y == 0 && plane.height == layout.status_bar.height);
    assert(plane.width == 480 && plane.height < 480 / 4);
    assert(!bar_is_visible(&ui, navigation_bar_mode(&ui)));
    assert(overlay_failures == 0);
    ui.transient_revealed = 0;
    update_system_overlay(&ui);
    assert(!g_plane.visible && overlay_allocations == 0);
    check_shade_levels(&ui, display);
    assert(overlay_failures == 0);
    lv_obj_delete(ui.root);
    lv_deinit();
    puts("Game Back/status overlays preserve alpha and coalesce shade slider updates");
    return 0;
}
