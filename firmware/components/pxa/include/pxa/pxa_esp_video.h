// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "pxa/video.h"
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Startup only; capability storage and context have board lifetime. */
bool pxa_esp_video_set_backend(const pxa_video_backend_t *);
const pxa_video_backend_t *pxa_esp_video_backend(void);
#ifdef __cplusplus
}
#endif
