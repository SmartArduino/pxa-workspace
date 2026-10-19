#include <pxa_board_common/boot_screen.h>
#include <lvgl.h>
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <vector>

struct Capture {
    unsigned width;
    std::vector<uint16_t> pixels;
};

static void Check(unsigned width, unsigned height) {
    auto* display = lv_display_create(width, height);
    Capture capture{width, std::vector<uint16_t>(width * height, 0xffff)};
    const size_t bytes = width * 16 * sizeof(uint16_t);
    void* draw = aligned_alloc(64, bytes);
    assert(draw);
    lv_display_set_user_data(display, &capture);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, draw, nullptr, bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, [](lv_display_t* d, const lv_area_t* area, uint8_t* p) {
        auto& c = *static_cast<Capture*>(lv_display_get_user_data(d));
        const unsigned stride = lv_draw_buf_width_to_stride(
            lv_area_get_width(area), LV_COLOR_FORMAT_RGB565) / 2;
        for (int y = area->y1; y <= area->y2; ++y)
            std::copy_n(reinterpret_cast<uint16_t*>(p) + (y - area->y1) * stride,
                lv_area_get_width(area), c.pixels.data() + y * c.width + area->x1);
        lv_display_flush_ready(d);
    });
    auto* root = lv_display_get_screen_active(display);
    pxa_board_common::BootScreen boot;
    boot.Show(nullptr);
    boot.Show(display);
    boot.Show(display);
    assert(lv_obj_get_child_count(root) == 1);
    lv_refr_now(display); // The very first flush must already be useful content.
    const auto dark = lv_color_to_u16(lv_color_hex(0x111827));
    const size_t dark_count = std::count(capture.pixels.begin(), capture.pixels.end(), dark);
    assert(dark_count > capture.pixels.size() * 95 / 100);
    assert(dark_count < capture.pixels.size()); // The built-in startup text rendered.
    assert(std::count(capture.pixels.begin(), capture.pixels.end(), 0xffff) == 0);
    auto* ready = lv_obj_create(root); // Simulate the newly created system UI.
    boot.Hide();
    boot.Hide();
    assert(lv_obj_get_child_count(root) == 1 && lv_obj_get_child(root, 0) == ready);
    lv_obj_delete(ready);
    lv_refr_now(display);
    assert(size_t(std::count(capture.pixels.begin(), capture.pixels.end(), dark)) == capture.pixels.size());
    lv_display_delete(display);
    free(draw);
    std::printf("Boot first frame %ux%u: dark background + built-in text; repeated show/hide and UI handover passed\n",
        width, height);
}

int main() {
    lv_init();
    Check(320, 240);
    Check(800, 480);
    lv_deinit();
}
