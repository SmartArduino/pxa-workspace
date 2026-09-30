#ifndef PXA_ESP_SYSTEM_OVERLAY_H
#define PXA_ESP_SYSTEM_OVERLAY_H

#include "lvgl.h"

void pxa_esp_system_overlay_bind(void);
void pxa_esp_system_overlay_add(lv_obj_t *object);
void pxa_esp_system_overlay_remove(lv_obj_t *object);
void pxa_esp_system_overlay_refresh(void);

#endif
