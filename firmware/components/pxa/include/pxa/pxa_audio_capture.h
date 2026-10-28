/* SPDX-License-Identifier: MIT */
#pragma once
#include <pxa/audio.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Board lifetime callbacks. OPEN/READ/CLOSE must not block. READ supplies one
 * mono 16 kHz raw Opus packet of frame_ms (20, 40 or 60); opening discards old input.
 * CLOSE stops delivery. Coprocessor sampling/power policy stays board-owned. */
void pxa_esp_audio_set_capture_source(
    void *context, uint16_t frame_ms, pxa_status_t (*open)(void *),
    int32_t (*read)(void *, uint8_t *, size_t), void (*close)(void *));
#ifdef __cplusplus
}
#endif
