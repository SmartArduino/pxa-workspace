// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "pxa/video.h"
#ifdef __cplusplus
extern "C" {
#endif
const pxa_video_backend_t *pxa_video_codecs_backend(void);
#if PXA_GAMEKIT_VERIFY_NATIVE_OUTPUT
bool pxa_gamekit_native_verify(uint16_t *logical, uint16_t *eager, uint16_t *fused, uint8_t rotation);
bool pxa_gamekit_native_verified(void);
#endif
#ifdef __cplusplus
}
#endif
