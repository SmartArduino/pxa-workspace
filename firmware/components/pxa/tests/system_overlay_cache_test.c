/* Real LVGL snapshots: a launcher overlay must disappear before a game's
 * first Surface becomes visible. Repeated unchanged shell refreshes stay cheap. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <lvgl.h>
#define ESP_PLATFORM 1
#ifndef PXA_SYSTEM_OVERLAY_SOURCE
#define PXA_SYSTEM_OVERLAY_SOURCE "../src/ui/pxa_esp_system_overlay.c"
#endif
#include PXA_SYSTEM_OVERLAY_SOURCE

static bool surface_visible;
static unsigned allocations, total_allocations, failures;
static unsigned provider_bindings, composition_wakeups;
void test_enter(void) {}
void test_leave(void) {}
void test_log(const char* tag, const char* format, ...) {
    (void)tag; (void)format; ++failures;
}
static void* allocate(size_t bytes) {
    unsigned char* base=malloc(bytes+LV_DRAW_BUF_ALIGN+sizeof(void*));
    assert(base);
    uintptr_t aligned=((uintptr_t)(base+sizeof(void*))+LV_DRAW_BUF_ALIGN-1)/LV_DRAW_BUF_ALIGN*LV_DRAW_BUF_ALIGN;
    memcpy((void*)(aligned-sizeof(void*)),&base,sizeof(base));
    ++allocations; ++total_allocations;
    return (void*)aligned;
}
void* heap_caps_malloc(size_t bytes,unsigned caps) { (void)caps;return allocate(bytes); }
void* heap_caps_calloc(size_t n,size_t bytes,unsigned caps) {
    (void)caps;void* p=allocate(n*bytes);memset(p,0,n*bytes);return p;
}
void heap_caps_free(void* p) {
    if(!p)return;
    void* base;memcpy(&base,(char*)p-sizeof(void*),sizeof(base));free(base);--allocations;
}
bool pxa_esp_surface_has_visible_surface(void) { return surface_visible; }
void pxa_esp_surface_set_system_alpha_provider(pxa_esp_surface_ui_alpha_provider_fn fn,void* context) {
    (void)fn;(void)context;
    ++provider_bindings;
}
void pxa_esp_surface_require_composition(void) { ++composition_wakeups; }
static void flush(lv_display_t* display,const lv_area_t* area,uint8_t* pixels) {
    (void)area;(void)pixels;lv_display_flush_ready(display);
}
int main(void) {
    lv_init();
    lv_display_t* display=lv_display_create(296,240);
    static uint8_t pixels[296*16*2];
    lv_display_set_color_format(display,LV_COLOR_FORMAT_RGB565_SWAPPED);
    lv_display_set_buffers(display,pixels,NULL,sizeof(pixels),LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display,flush);
    lv_obj_t* bar=lv_obj_create(lv_layer_top());
    lv_obj_set_pos(bar,25,0);lv_obj_set_size(bar,240,24);
    lv_obj_set_style_bg_color(bar,lv_color_hex(0xff0000),0);
    lv_obj_set_style_bg_opa(bar,LV_OPA_COVER,0);
    lv_obj_t* objects[]={bar};
    pxa_esp_system_overlay_set_reference_objects(objects,1);
    assert(provider_bindings == 1); /* Product shell needs no private bind API. */
    pxa_esp_system_overlay_bind();
    assert(provider_bindings == 1);
    assert(g_plane.visible&&g_plane.pixels&&g_plane.alpha);
    unsigned unchanged=total_allocations;
    for(unsigned i=0;i<30;++i)pxa_esp_system_overlay_set_reference_objects(objects,1);
    assert(total_allocations==unchanged);
    /* The fullscreen config arrives while LVGL still owns the panel. */
    lv_obj_set_hidden(bar,true);
    pxa_esp_system_overlay_set_reference_objects(NULL,0);
    assert(!g_plane.visible&&!g_plane_memory&&allocations==0);
    assert(total_allocations==unchanged);
    surface_visible=true;
    pxa_esp_surface_ui_alpha_plane_t plane;
    assert(provide_plane(NULL,&plane)&&!plane.visible);
    /* Real system chrome still composites normally above an active game. */
    lv_obj_set_hidden(bar,false);
    pxa_esp_system_overlay_set_reference_objects(objects,1);
    assert(g_plane.visible&&g_plane.pixels);
    pxa_esp_system_overlay_set_reference_objects(NULL,0);
    assert(!g_plane.visible&&allocations==0);
    surface_visible=false;
    pxa_esp_system_overlay_add(bar);
    assert(g_plane.visible);
    pxa_esp_system_overlay_remove(bar);
    assert(!g_plane.visible&&allocations==0&&failures==0);
    unsigned idle_wakeups = composition_wakeups;
    for(unsigned i=0;i<30;++i)pxa_esp_system_overlay_set_reference_objects(NULL,0);
    assert(composition_wakeups == idle_wakeups);

    /* Mixed-parent native bars must remain behind top-layer dialogs even
     * when publication order is reversed. More than eight roots are legal. */
    surface_visible=true;
    lv_obj_t* mixed[12];
    mixed[0]=lv_obj_create(lv_layer_top());
    lv_obj_set_pos(mixed[0],40,40);lv_obj_set_size(mixed[0],20,20);
    lv_obj_set_style_bg_color(mixed[0],lv_color_hex(0x0000ff),0);
    lv_obj_set_style_bg_opa(mixed[0],LV_OPA_COVER,0);
    lv_obj_set_style_border_width(mixed[0],0,0);
    for(unsigned i=1;i<12;++i) {
        mixed[i]=lv_obj_create(lv_screen_active());
        lv_obj_set_pos(mixed[i],40,40);lv_obj_set_size(mixed[i],20,20);
        lv_obj_set_style_bg_color(mixed[i],lv_color_hex(0xff0000),0);
        lv_obj_set_style_bg_opa(mixed[i],LV_OPA_COVER,0);
        lv_obj_set_style_border_width(mixed[i],0,0);
    }
    pxa_esp_system_overlay_set_reference_objects(mixed,12);
    assert(g_reference_count == 12 && g_plane.visible && g_plane.pixels);
    size_t center=(size_t)(50-g_plane.y)*g_plane.width+(50-g_plane.x);
    assert(g_plane.pixels[center]==0x001f && g_plane.alpha[center]==255);
    pxa_esp_system_overlay_set_reference_objects(NULL,0);
    for(unsigned i=0;i<12;++i)lv_obj_delete(mixed[i]);
    assert(allocations==0&&failures==0);
    lv_obj_delete(bar);lv_deinit();
    puts("Overlay cache: automatic product binding; empty refresh has no wakeups; fullscreen clears launcher bars; mixed-parent order and 12 roots; no leaked plane memory");
}
