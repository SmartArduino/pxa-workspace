// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "pxa/audio.h"

#ifdef __cplusplus
extern "C" {
#endif

int32_t pxa_esp_encoded_audio_io(void *context, uint64_t session,
    const pxa_audio_format_t *format, uint32_t operation, uint8_t *data,
    size_t size, pxa_audio_submit_fn submit);
/* Call from the output task after releasing the mixer lock. */
void pxa_esp_encoded_audio_writable(void);
/* Zero retires every producer. Call before taking the PCM mixer's lock. */
void pxa_esp_encoded_audio_detach(uint64_t session);

#ifdef __cplusplus
}
#endif
