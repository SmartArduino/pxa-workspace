/* SPDX-License-Identifier: MIT. ESP32-S3 has no PPA peripheral. */
#include <stdint.h>
#include <stdbool.h>
bool aster_idf_ppa_fill_rgb565(uint16_t *p,uint32_t pitch,uint32_t height,
 uint32_t x,uint32_t y,uint32_t w,uint32_t h,uint16_t c)
{(void)p;(void)pitch;(void)height;(void)x;(void)y;(void)w;(void)h;(void)c;return false;}
int aster_idf_ppa_fill_init(void){return 0;}
void aster_idf_ppa_fill_reset(void){}
void aster_idf_ppa_fill_report(void){}
