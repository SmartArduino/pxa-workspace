// SPDX-License-Identifier: Apache-2.0
#pragma once
#if PXA_ENABLE_VOICE_TRIGGER
#include <stdbool.h>
#include <pxa_ext/voice_trigger.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Register once at board startup. Callback context must have board lifetime.
 * No registration means the extension is absent, not an empty standard service. */
bool pxa_esp_voice_trigger_set_backend(const pxa_vt_backend_t *backend);
const pxa_vt_backend_t *pxa_esp_voice_trigger_backend(void);
#ifdef __cplusplus
}
#endif
#endif
