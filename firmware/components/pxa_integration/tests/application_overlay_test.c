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
    ui.system_overlay_objects_changed = overlay_objects;
    pxsys_theme_snapshot_init(&ui.theme, PXSYS_COLOR_SCHEME_DARK);
    pxa_esp_system_overlay_bind();
    /* Use the actual ESP overlay snapshot/provider with the device's 64-byte
     * draw-buffer rules. Every Back progress size must preserve game pixels
     * outside the small alpha plane instead of publishing an opaque fallback. */
    for (int distance = 4; distance <= 100; ++distance) {
        navigation_back_indicator_update(&ui, 12, 12, 12 + distance, 240);
        pxa_esp_surface_ui_alpha_plane_t plane;
        assert(provide_plane(NULL, &plane));
        assert(plane.visible && plane.pixels != NULL && plane.alpha != NULL);
        assert(plane.width < 64 && plane.height < 96);
        assert(plane.x > 0 && plane.y > 0);
        assert(plane.alpha[0] == 0);
        assert(!ui.application_presentation_hidden);
    }
    assert(overlay_failures == 0);
    navigation_back_indicator_reset(&ui);
    assert(!g_plane.visible && overlay_allocations == 0);
    lv_obj_delete(ui.root);
    lv_deinit();
    puts("Game Back overlay: all progress sizes preserve alpha with 64-byte alignment");
    return 0;
}
