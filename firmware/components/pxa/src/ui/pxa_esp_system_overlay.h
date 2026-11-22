#ifndef PXA_ESP_SYSTEM_OVERLAY_H
#define PXA_ESP_SYSTEM_OVERLAY_H

#include "lvgl.h"

void pxa_esp_system_overlay_bind(void);
void pxa_esp_system_overlay_add(lv_obj_t *object);
void pxa_esp_system_overlay_remove(lv_obj_t *object);
/* The launch cover occludes the whole display in the LVGL framebuffer. */
void pxa_esp_system_overlay_set_fullscreen_opaque(bool visible);
void pxa_esp_system_overlay_refresh(void);
/* Legacy shells retain their opaque fallback until they opt into trusted roots. */
bool pxa_esp_system_overlay_reference_enabled(void);

#endif
