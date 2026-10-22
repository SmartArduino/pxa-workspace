#ifndef PXA_ESP_FONT_SOURCE_H
#define PXA_ESP_FONT_SOURCE_H
#include "pxa/status.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct pxa_esp_font_source pxa_esp_font_source_t;
/* Native font consumers acquire one source before constructing any faces or
 * sized-font caches. Its read-only FreeType name binds the original mount
 * epoch; it can never reopen a replacement card or a parent filesystem.
 * Release after destroying all faces/caches that borrow the name. */
pxa_status_t pxa_esp_font_source_acquire(const char *native_path,pxa_esp_font_source_t **);
const char *pxa_esp_font_source_path(const pxa_esp_font_source_t *);
pxa_status_t pxa_esp_font_source_release(pxa_esp_font_source_t **);
#ifdef __cplusplus
}
#endif
#endif
