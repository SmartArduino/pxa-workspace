#ifndef PXA_ESP_SYSTEM_FONT_H
#define PXA_ESP_SYSTEM_FONT_H
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Board bring-up only, after mounting the explicitly selected volume and
 * before creating fonts. No directory discovery or automatic formatting.
 * The selected buffer is stable for the lifetime of the boot. */
bool pxa_esp_system_font_prepare(const char *mounted_volume, bool reclaim_internal);
const char *pxa_esp_system_font_path(void);
#ifdef __cplusplus
}
#endif
#endif
