#ifndef PXA_ESP_AUDIO_H
#define PXA_ESP_AUDIO_H

#include <stddef.h>
#include <stdint.h>

#include "pxa/audio.h"
#include "pxa/package.h"
#include "pxa/pxa_host.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PXA_ESP_AUDIO_VOICE_COUNT UINT16_C(3)
#define PXA_ESP_AUDIO_ASSET_PATH_MAX UINT16_C(512)

typedef struct {
    uint64_t submitted_frames;
    uint64_t rendered_frames;
    uint64_t queue_full_frames;
    uint64_t stale_frames;
    uint64_t sink_rejected_frames;
    uint64_t tone_commands;
    uint64_t tone_frames;
    uint64_t tone_dropped_commands;
    uint64_t voice_exhaustions;
    uint64_t asset_play_commands;
    uint64_t asset_control_commands;
    uint64_t asset_command_failures;
    uint32_t queue_capacity;
    uint32_t queued_frames;
    uint32_t peak_queued_frames;
    uint32_t queue_storage_bytes;
    uint32_t task_stack_bytes;
    uint16_t active_sessions;
} pxa_esp_audio_snapshot_t;

int pxa_esp_audio_initialize(void);
void pxa_esp_audio_deinitialize(void);

void pxa_esp_audio_reset_sessions(void);
void pxa_esp_audio_backend(pxa_audio_backend_t *output);
void pxa_esp_audio_snapshot(pxa_esp_audio_snapshot_t *output);
int pxa_esp_audio_bind_package(const pxa_package_manifest_t *manifest,
                               const char *package_root);

void pxa_esp_audio_set_sink(pxa_host_audio_submit_fn submit,
                            pxa_host_audio_flush_fn flush, void *context);
void pxa_esp_audio_set_asset_sink(
    pxa_host_audio_asset_play_fn play,
    pxa_host_audio_asset_control_fn control, void *context);

#ifdef __cplusplus
}
#endif

#endif
