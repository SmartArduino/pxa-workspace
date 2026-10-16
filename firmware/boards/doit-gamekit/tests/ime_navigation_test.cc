#include "doit_gamekit_navigation.h"
#include "pxa/lvgl/pxa_lvgl_ime.h"
#include <cassert>
#include <cstdio>
#include <cstring>

using namespace doit_gamekit;
alignas(64) static uint32_t pixels[320 * 240];
static uint32_t tick;
static void Update(KeyNavigation& nav, lv_obj_t* scope, uint32_t buttons = 0) {
    tick += 20; lv_tick_inc(20); nav.Update(scope, true, buttons, tick);
}
static void Tap(KeyNavigation& nav, lv_obj_t* scope, uint32_t button) {
    Update(nav, scope, button); Update(nav, scope);
}
static const char* Selected(KeyNavigation& nav) {
    return lv_buttonmatrix_get_button_text(nav.focused(), lv_buttonmatrix_get_selected_button(nav.focused()));
}
static lv_obj_t* FindKey(lv_obj_t* root, const char* text, uint32_t& index) {
    if (lv_obj_is_hidden(root)) return nullptr;
    if (lv_obj_has_class(root, &lv_buttonmatrix_class))
        for (index = 0; lv_buttonmatrix_get_button_text(root, index); ++index)
            if (!std::strcmp(lv_buttonmatrix_get_button_text(root, index), text)) return root;
    for (uint32_t i = 0; i < lv_obj_get_child_count(root); ++i)
        if (auto* object = FindKey(lv_obj_get_child(root, i), text, index)) return object;
    return nullptr;
}
static void Choose(KeyNavigation& nav, lv_obj_t* scope, const char* text) {
    uint32_t index = 0;
    auto* matrix = FindKey(scope, text, index);
    assert(matrix);
    for (unsigned step = 0; step < 16 && nav.focused() != matrix; ++step) Tap(nav, scope, R);
    assert(nav.focused() == matrix);
    lv_buttonmatrix_set_selected_button(matrix, index);
    Tap(nav, scope, A); // Real keypad press/release, not a synthetic VALUE_CHANGED.
}
static void Highlight(lv_display_t* display, KeyNavigation& nav) {
    lv_refr_now(display);
    auto* matrix = nav.focused();
    const auto index = lv_buttonmatrix_get_selected_button(matrix);
    const auto cell = reinterpret_cast<lv_buttonmatrix_t*>(matrix)->button_areas[index];
    lv_area_t box; lv_obj_get_coords(matrix, &box);
    const int x = box.x1 + (cell.x1 + cell.x2) / 2, y = box.y1 + (cell.y1 + cell.y2) / 2;
    for (auto offset : {(box.y1 + cell.y1) * 320 + x, (box.y1 + cell.y2) * 320 + x,
                        y * 320 + box.x1 + cell.x1, y * 320 + box.x1 + cell.x2})
        assert((pixels[offset] & 0xffffff) == 0xffb74d);
    // The old implementation highlighted this enclosing panel instead.
    assert((pixels[box.y1 * 320 + (box.x1 + box.x2) / 2] & 0xffffff) != 0xffb74d);
}
struct Provider { unsigned calls = 0; uint16_t action = 0; uint32_t value = 0; char text[8]{}; };
static pxa_status_t Enqueue(void* context, uint64_t, uint16_t action, uint8_t, uint8_t,
                           uint32_t value, pxa_bytes_t text) {
    auto& provider = *static_cast<Provider*>(context);
    ++provider.calls; provider.action = action; provider.value = value;
    assert(text.size < sizeof(provider.text));
    if (text.size) std::memcpy(provider.text, text.data, text.size);
    provider.text[text.size] = 0;
    return PXA_STATUS_OK;
}
static void Focus(void*, uint64_t, bool, bool) {}

int main() {
    lv_init();
    auto* display = lv_display_create(320, 240);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_XRGB8888);
    lv_display_set_buffers(display, pixels, nullptr, sizeof(pixels), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display, [](lv_display_t* d, const lv_area_t*, uint8_t*) { lv_display_flush_ready(d); });
    auto* pointer = lv_indev_create();
    lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_mode(pointer, LV_INDEV_MODE_EVENT);
    auto* input = lv_textarea_create(lv_screen_active());
    lv_obj_set_pos(input, 0, 0); lv_obj_set_size(input, 320, 32);
    lv_textarea_set_one_line(input, true); lv_textarea_set_text(input, "");
    KeyNavigation nav; assert(nav.Initialize(display));
    pxa_lvgl_ime_config_t config{};
    config.struct_size = sizeof(config); config.parent = lv_layer_top();
    config.environment.width = 320; config.environment.height = 240; config.environment.density_q16 = 65536;
    config.theme.body_font = config.theme.caption_font = config.theme.icon_font = LV_FONT_DEFAULT;
    config.preferences = {PXA_IME_LAYOUT_FULL, PXA_IME_ENGLISH}; config.focus = Focus;
    auto* ime = pxa_lvgl_ime_create(&config); assert(ime);
    assert(pxa_lvgl_ime_focus(ime, input) == PXA_STATUS_OK);
    auto* scope = static_cast<lv_obj_t*>(pxa_lvgl_ime_root(ime));
    Update(nav, scope);
    Tap(nav, scope, Down); assert(!std::strcmp(Selected(nav), "q"));
    Highlight(display, nav);
    Tap(nav, scope, Right); assert(!std::strcmp(Selected(nav), "w"));
    Highlight(display, nav); Tap(nav, scope, A);
    assert(!std::strcmp(lv_textarea_get_text(input), "w"));
    Tap(nav, scope, Down); assert(!std::strcmp(Selected(nav), "s"));
    Tap(nav, scope, Down); auto* typing = nav.focused();
    Tap(nav, scope, Down); assert(nav.focused() != typing); // Leave the bottom typing row for the toolbar.
    Highlight(display, nav); Tap(nav, scope, Up); assert(nav.focused() == typing);
    Choose(nav, scope, "123"); Choose(nav, scope, "7");
    assert(!std::strcmp(lv_textarea_get_text(input), "w7"));
    Choose(nav, scope, "返回"); Choose(nav, scope, LV_SYMBOL_BACKSPACE);
    assert(!std::strcmp(lv_textarea_get_text(input), "w"));
    nav.Update(nullptr, false, 0, tick);
    pxa_lvgl_ime_destroy(ime);

    // The local decoder and a PXA Provider use exactly this same frontend.
    // Exercise Provider enqueue/present rather than substituting local edits.
    Provider provider;
    config.preferences.language = PXA_IME_CHINESE; config.input = Enqueue; config.context = &provider;
    lv_textarea_set_text(input, "");
    ime = pxa_lvgl_ime_create(&config); assert(ime);
    assert(pxa_lvgl_ime_focus(ime, input) == PXA_STATUS_OK);
    scope = static_cast<lv_obj_t*>(pxa_lvgl_ime_root(ime));
    pxa_ime_frame_t reply{}; reply.session = 1; reply.sequence = 1;
    assert(pxa_lvgl_ime_present(ime, pxa_lvgl_ime_generation(ime), &reply) == PXA_STATUS_OK);
    Update(nav, scope); Choose(nav, scope, "n");
    assert(provider.calls == 1 && provider.action == PXA_IME_TEXT && !std::strcmp(provider.text, "n"));
    const uint8_t candidate[] = {3, 0, 0xe4, 0xbd, 0xa0}; // UTF-8 你
    const uint8_t preedit[] = {'n'};
    reply.sequence = 2; reply.preedit = {preedit, sizeof(preedit)};
    reply.candidate_count = 1; reply.items = {candidate, sizeof(candidate)};
    assert(pxa_lvgl_ime_present(ime, pxa_lvgl_ime_generation(ime), &reply) == PXA_STATUS_OK);
    Update(nav, scope); Choose(nav, scope, "你");
    assert(provider.calls == 2 && provider.action == PXA_IME_CHOOSE && provider.value == 0);
    reply = {}; reply.session = 1; reply.sequence = 3; reply.edit = PXA_IME_EDIT_INSERT;
    reply.commit = {candidate + 2, 3};
    assert(pxa_lvgl_ime_present(ime, pxa_lvgl_ime_generation(ime), &reply) == PXA_STATUS_OK);
    assert(!std::strcmp(lv_textarea_get_text(input), "你"));
    nav.Update(nullptr, false, 0, tick); pxa_lvgl_ime_destroy(ime);
    nav.Deinitialize(); lv_indev_delete(pointer); lv_deinit();
    std::puts("DOIT GameKit local and PXA Provider keypad IME tests passed");
}
