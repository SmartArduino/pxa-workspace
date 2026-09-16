#include "pxa_esp_audio.h"

#if defined(ESP_PLATFORM)

#include <limits.h>
#include <math.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define PXA_ESP_AUDIO_TAG "PxaAudio"
#define PXA_ESP_AUDIO_SAMPLE_RATE 16000
#define PXA_ESP_AUDIO_FRAME_MS 20
#define PXA_ESP_AUDIO_FRAME_SAMPLES \
    (PXA_ESP_AUDIO_SAMPLE_RATE * PXA_ESP_AUDIO_FRAME_MS / 1000)
#define PXA_ESP_AUDIO_QUEUE_LENGTH 12
#define PXA_ESP_AUDIO_TASK_STACK_SIZE 4096
#define PXA_ESP_AUDIO_TASK_PRIORITY 5

enum {
    PXA_ESP_AUDIO_PACKET_PCM = 0,
    PXA_ESP_AUDIO_PACKET_FLUSH = 1,
    PXA_ESP_AUDIO_PACKET_TONE = 2,
};

typedef struct {
    uint64_t provider_session;
    uint32_t epoch;
    float linear_gain;
    uint16_t samples;
    uint16_t frequency_hz;
    uint16_t tone_samples;
    uint16_t attack_samples;
    uint16_t release_samples;
    uint16_t delay_samples;
    uint8_t kind;
    uint8_t waveform;
    int16_t pcm[PXA_ESP_AUDIO_FRAME_SAMPLES];
} pxa_esp_audio_packet_t;

typedef struct {
    uint64_t provider_session;
    uint64_t submitted_samples;
    uint64_t accepted_samples;
    uint32_t queued_samples;
    uint32_t epoch;
    float linear_gain;
    uint8_t active;
    uint8_t graph_committed;
    uint8_t closing;
    uint8_t asset_active;
} pxa_esp_audio_slot_t;

typedef struct {
    pxa_host_audio_submit_fn submit;
    pxa_host_audio_flush_fn flush;
    void *context;
} pxa_esp_audio_sink_t;

typedef struct {
    pxa_host_audio_asset_play_fn play;
    pxa_host_audio_asset_control_fn control;
    void *context;
} pxa_esp_audio_asset_sink_t;

typedef struct {
    QueueHandle_t queue;
    TaskHandle_t task;
    portMUX_TYPE backend_lock;
    portMUX_TYPE sink_lock;
    pxa_esp_audio_slot_t slots[PXA_ESP_AUDIO_VOICE_COUNT];
    pxa_esp_audio_sink_t sink;
    pxa_esp_audio_asset_sink_t asset_sink;
    const pxa_package_manifest_t *package_manifest;
    char package_root[PXA_ESP_AUDIO_ASSET_PATH_MAX];
    uint64_t next_provider_session;
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
    uint32_t peak_queued_frames;
} pxa_esp_audio_state_t;

static pxa_esp_audio_state_t g_audio = {
    .backend_lock = portMUX_INITIALIZER_UNLOCKED,
    .sink_lock = portMUX_INITIALIZER_UNLOCKED,
};

static size_t bounded_string_size(const char *value, size_t capacity) {
    size_t size = 0;
    if (value == NULL) return capacity;
    while (size < capacity && value[size] != '\0') ++size;
    return size;
}

static int slot_index_locked(const pxa_esp_audio_state_t *audio,
                             uint64_t provider_session) {
    uint16_t index;
    for (index = 0; index < PXA_ESP_AUDIO_VOICE_COUNT; ++index) {
        const pxa_esp_audio_slot_t *slot = &audio->slots[index];
        if (slot->active && slot->provider_session == provider_session) {
            return (int)index;
        }
    }
    return -1;
}

static int sink_snapshot(pxa_esp_audio_state_t *audio,
                         pxa_esp_audio_sink_t *sink) {
    portENTER_CRITICAL(&audio->sink_lock);
    *sink = audio->sink;
    portEXIT_CRITICAL(&audio->sink_lock);
    return sink->submit != NULL;
}

static int asset_sink_snapshot(pxa_esp_audio_state_t *audio,
                               pxa_esp_audio_asset_sink_t *sink) {
    portENTER_CRITICAL(&audio->sink_lock);
    *sink = audio->asset_sink;
    portEXIT_CRITICAL(&audio->sink_lock);
    return sink->play != NULL && sink->control != NULL;
}

static int asset_path_is_ogg(const uint8_t *path, size_t size) {
    static const char suffix[] = ".ogg";
    return path != NULL && size > sizeof(suffix) - 1u &&
           memcmp(path + size - (sizeof(suffix) - 1u), suffix,
                  sizeof(suffix) - 1u) == 0;
}

static pxa_status_t audio_open(void *context, uint16_t usage,
                               pxa_audio_format_t *format,
                               uint64_t *provider_session) {
    pxa_esp_audio_state_t *audio = (pxa_esp_audio_state_t *)context;
    pxa_esp_audio_sink_t sink;
    uint16_t index;
    if (audio != &g_audio || usage != PXA_AUDIO_USAGE_MEDIA ||
        format == NULL || provider_session == NULL ||
        !sink_snapshot(audio, &sink)) {
        return PXA_STATUS_UNSUPPORTED;
    }
    if (!pxa_esp_audio_initialize()) return PXA_STATUS_RESOURCE_LIMIT;
    portENTER_CRITICAL(&audio->backend_lock);
    for (index = 0; index < PXA_ESP_AUDIO_VOICE_COUNT; ++index) {
        pxa_esp_audio_slot_t *slot = &audio->slots[index];
        if (!slot->active && !slot->closing) {
            uint64_t session = ++audio->next_provider_session;
            if (session == 0) session = ++audio->next_provider_session;
            memset(slot, 0, sizeof(*slot));
            slot->active = 1;
            slot->provider_session = session;
            *provider_session = session;
            format->sample_rate = PXA_ESP_AUDIO_SAMPLE_RATE;
            format->channels = 1;
            format->frame_ms = PXA_ESP_AUDIO_FRAME_MS;
            portEXIT_CRITICAL(&audio->backend_lock);
            return PXA_STATUS_OK;
        }
    }
    ++audio->voice_exhaustions;
    portEXIT_CRITICAL(&audio->backend_lock);
    return PXA_STATUS_RESOURCE_LIMIT;
}

static pxa_status_t audio_commit(void *context, uint64_t provider_session,
                                 const pxa_audio_graph_t *graph) {
    pxa_esp_audio_state_t *audio = (pxa_esp_audio_state_t *)context;
    float linear_gain;
    int slot_index;
    if (audio != &g_audio || graph == NULL ||
        graph->route != PXA_AUDIO_ROUTE_SPEAKER) {
        return PXA_STATUS_INVALID_ARGUMENT;
    }
    linear_gain = graph->gain_db_q8 == 0
                      ? 1.0f
                      : powf(10.0f, (float)graph->gain_db_q8 / 5120.0f);
    portENTER_CRITICAL(&audio->backend_lock);
    slot_index = slot_index_locked(audio, provider_session);
    if (slot_index >= 0) {
        pxa_esp_audio_slot_t *slot = &audio->slots[slot_index];
        slot->linear_gain = linear_gain;
        slot->graph_committed = 1;
    }
    portEXIT_CRITICAL(&audio->backend_lock);
    if (slot_index < 0) return PXA_STATUS_NOT_FOUND;
    if (graph->eq_band_count != 0) {
        ESP_LOGW(PXA_ESP_AUDIO_TAG,
                 "PXA equalizer graph accepted but not applied on the Game bus");
    }
    return PXA_STATUS_OK;
}

static pxa_status_t audio_submit(void *context, uint64_t provider_session,
                                 const uint8_t *pcm, size_t size) {
    pxa_esp_audio_state_t *audio = (pxa_esp_audio_state_t *)context;
    pxa_esp_audio_packet_t packet;
    UBaseType_t queued;
    BaseType_t sent;
    int slot_index;
    if (audio != &g_audio || pcm == NULL || size == 0 ||
        size > sizeof(packet.pcm) || (size % sizeof(packet.pcm[0])) != 0 ||
        audio->queue == NULL) {
        return PXA_STATUS_INVALID_ARGUMENT;
    }
    portENTER_CRITICAL(&audio->backend_lock);
    slot_index = slot_index_locked(audio, provider_session);
    if (slot_index >= 0 && !audio->slots[slot_index].graph_committed) {
        slot_index = -2;
    }
    if (slot_index >= 0) {
        packet.provider_session = provider_session;
        packet.epoch = audio->slots[slot_index].epoch;
        packet.linear_gain = audio->slots[slot_index].linear_gain;
    }
    portEXIT_CRITICAL(&audio->backend_lock);
    if (slot_index == -2) return PXA_STATUS_BAD_STATE;
    if (slot_index < 0) return PXA_STATUS_NOT_FOUND;
    packet.samples = (uint16_t)(size / sizeof(packet.pcm[0]));
    packet.kind = PXA_ESP_AUDIO_PACKET_PCM;
    memcpy(packet.pcm, pcm, size);
    sent = xQueueSendToBack(audio->queue, &packet, 0);
    queued = sent == pdTRUE ? uxQueueMessagesWaiting(audio->queue) : 0;
    portENTER_CRITICAL(&audio->backend_lock);
    if (sent == pdTRUE) {
        audio->slots[slot_index].submitted_samples += packet.samples;
        audio->slots[slot_index].queued_samples += packet.samples;
        audio->submitted_frames++;
        if (queued > audio->peak_queued_frames) {
            audio->peak_queued_frames = (uint32_t)queued;
        }
    } else {
        audio->queue_full_frames++;
    }
    portEXIT_CRITICAL(&audio->backend_lock);
    return sent == pdTRUE ? PXA_STATUS_OK : PXA_STATUS_WOULD_BLOCK;
}

static pxa_status_t audio_play_tone(void *context,
                                    uint64_t provider_session,
                                    const pxa_audio_tone_t *tone) {
    pxa_esp_audio_state_t *audio = (pxa_esp_audio_state_t *)context;
    pxa_esp_audio_packet_t packet;
    UBaseType_t queued;
    BaseType_t sent;
    int slot_index;
    if (audio != &g_audio || tone == NULL || audio->queue == NULL) {
        return PXA_STATUS_INVALID_ARGUMENT;
    }
    memset(&packet, 0, sizeof(packet));
    portENTER_CRITICAL(&audio->backend_lock);
    slot_index = slot_index_locked(audio, provider_session);
    if (slot_index >= 0 && !audio->slots[slot_index].graph_committed) {
        slot_index = -2;
    }
    if (slot_index >= 0) {
        packet.provider_session = provider_session;
        packet.epoch = audio->slots[slot_index].epoch;
        packet.linear_gain = audio->slots[slot_index].linear_gain;
    }
    portEXIT_CRITICAL(&audio->backend_lock);
    if (slot_index == -2) return PXA_STATUS_BAD_STATE;
    if (slot_index < 0) return PXA_STATUS_NOT_FOUND;
    packet.kind = PXA_ESP_AUDIO_PACKET_TONE;
    packet.waveform = tone->waveform;
    packet.frequency_hz = tone->frequency_hz;
    packet.tone_samples = (uint16_t)(
        (uint32_t)PXA_ESP_AUDIO_SAMPLE_RATE * tone->duration_ms / 1000u);
    packet.attack_samples = (uint16_t)(
        (uint32_t)PXA_ESP_AUDIO_SAMPLE_RATE * tone->attack_ms / 1000u);
    packet.release_samples = (uint16_t)(
        (uint32_t)PXA_ESP_AUDIO_SAMPLE_RATE * tone->release_ms / 1000u);
    packet.delay_samples = (uint16_t)(
        (uint32_t)PXA_ESP_AUDIO_SAMPLE_RATE * tone->delay_ms / 1000u);
    packet.samples = packet.tone_samples + packet.delay_samples;
    packet.linear_gain *= tone->gain_db_q8 == 0
                              ? 1.0f
                              : powf(10.0f,
                                     (float)tone->gain_db_q8 / 5120.0f);
    sent = xQueueSendToBack(audio->queue, &packet, 0);
    queued = sent == pdTRUE ? uxQueueMessagesWaiting(audio->queue) : 0;
    portENTER_CRITICAL(&audio->backend_lock);
    if (sent == pdTRUE) {
        audio->slots[slot_index].submitted_samples += packet.samples;
        audio->slots[slot_index].queued_samples += packet.samples;
        ++audio->submitted_frames;
        ++audio->tone_commands;
        if (queued > audio->peak_queued_frames)
            audio->peak_queued_frames = (uint32_t)queued;
    } else {
        ++audio->queue_full_frames;
        ++audio->tone_dropped_commands;
    }
    portEXIT_CRITICAL(&audio->backend_lock);
    return sent == pdTRUE ? PXA_STATUS_OK : PXA_STATUS_WOULD_BLOCK;
}

static pxa_status_t audio_play_asset(void *context,
                                     uint64_t provider_session,
                                     const pxa_audio_asset_t *asset) {
    pxa_esp_audio_state_t *audio = (pxa_esp_audio_state_t *)context;
    pxa_esp_audio_asset_sink_t sink;
    const pxa_package_manifest_t *manifest;
    const pxa_package_file_t *file = NULL;
    char absolute_path[PXA_ESP_AUDIO_ASSET_PATH_MAX];
    size_t root_size;
    int slot_index;
    int accepted;
    if (audio != &g_audio || asset == NULL || asset->path == NULL ||
        asset->path_size == 0 || !asset_path_is_ogg(asset->path,
                                                    asset->path_size) ||
        !asset_sink_snapshot(audio, &sink)) {
        return PXA_STATUS_UNSUPPORTED;
    }
    portENTER_CRITICAL(&audio->backend_lock);
    slot_index = slot_index_locked(audio, provider_session);
    if (slot_index >= 0 && !audio->slots[slot_index].graph_committed)
        slot_index = -2;
    manifest = audio->package_manifest;
    root_size = bounded_string_size(audio->package_root,
                                    sizeof(audio->package_root));
    if (manifest == NULL || root_size == 0 ||
        root_size + 1u + asset->path_size + 1u > sizeof(absolute_path)) {
        if (slot_index >= 0) slot_index = -3;
    } else if (slot_index >= 0) {
        memcpy(absolute_path, audio->package_root, root_size);
    }
    portEXIT_CRITICAL(&audio->backend_lock);
    if (slot_index == -2) return PXA_STATUS_BAD_STATE;
    if (slot_index == -3) return PXA_STATUS_NOT_FOUND;
    if (slot_index < 0) return PXA_STATUS_NOT_FOUND;
    file = pxa_package_file_find(
        manifest, (pxa_bytes_t){asset->path, asset->path_size});
    if (file == NULL) return PXA_STATUS_NOT_FOUND;
    absolute_path[root_size] = '/';
    memcpy(absolute_path + root_size + 1u, asset->path,
           asset->path_size);
    absolute_path[root_size + 1u + asset->path_size] = '\0';
    portENTER_CRITICAL(&audio->backend_lock);
    if (audio->package_manifest != manifest ||
        slot_index_locked(audio, provider_session) != slot_index) {
        slot_index = -1;
    }
    portEXIT_CRITICAL(&audio->backend_lock);
    if (slot_index < 0) return PXA_STATUS_NOT_FOUND;
    accepted = sink.play(sink.context, (uint8_t)slot_index, absolute_path,
                         (asset->flags & PXA_AUDIO_ASSET_LOOP) != 0,
                         asset->gain_db_q8);
    portENTER_CRITICAL(&audio->backend_lock);
    ++audio->asset_play_commands;
    if (accepted && slot_index_locked(audio, provider_session) == slot_index) {
        audio->slots[slot_index].asset_active = 1;
    } else if (!accepted) {
        ++audio->asset_command_failures;
    }
    portEXIT_CRITICAL(&audio->backend_lock);
    return accepted ? PXA_STATUS_OK : PXA_STATUS_WOULD_BLOCK;
}

static pxa_status_t audio_control_asset(
    void *context, uint64_t provider_session,
    const pxa_audio_asset_control_t *control) {
    pxa_esp_audio_state_t *audio = (pxa_esp_audio_state_t *)context;
    pxa_esp_audio_asset_sink_t sink;
    int slot_index;
    int accepted;
    if (audio != &g_audio || control == NULL ||
        !asset_sink_snapshot(audio, &sink)) {
        return PXA_STATUS_UNSUPPORTED;
    }
    portENTER_CRITICAL(&audio->backend_lock);
    slot_index = slot_index_locked(audio, provider_session);
    if (slot_index >= 0 && !audio->slots[slot_index].asset_active)
        slot_index = -2;
    portEXIT_CRITICAL(&audio->backend_lock);
    if (slot_index == -2) return PXA_STATUS_BAD_STATE;
    if (slot_index < 0) return PXA_STATUS_NOT_FOUND;
    accepted = sink.control(sink.context, (uint8_t)slot_index,
                            control->action, control->gain_db_q8);
    portENTER_CRITICAL(&audio->backend_lock);
    ++audio->asset_control_commands;
    if (accepted && control->action == PXA_AUDIO_ASSET_STOP &&
        slot_index_locked(audio, provider_session) == slot_index) {
        audio->slots[slot_index].asset_active = 0;
    } else if (!accepted) {
        ++audio->asset_command_failures;
    }
    portEXIT_CRITICAL(&audio->backend_lock);
    return accepted ? PXA_STATUS_OK : PXA_STATUS_WOULD_BLOCK;
}

static pxa_status_t audio_query(void *context, uint64_t provider_session,
                                pxa_audio_state_t *state) {
    pxa_esp_audio_state_t *audio = (pxa_esp_audio_state_t *)context;
    int slot_index;
    if (audio != &g_audio || state == NULL)
        return PXA_STATUS_INVALID_ARGUMENT;
    portENTER_CRITICAL(&audio->backend_lock);
    slot_index = slot_index_locked(audio, provider_session);
    if (slot_index >= 0) {
        const pxa_esp_audio_slot_t *slot = &audio->slots[slot_index];
        state->submitted_samples = slot->submitted_samples;
        state->accepted_samples = slot->accepted_samples;
        state->queued_samples = slot->queued_samples;
        state->flags = PXA_AUDIO_STATE_ACCEPTED_IS_SINK_SUBMITTED;
    }
    portEXIT_CRITICAL(&audio->backend_lock);
    return slot_index >= 0 ? PXA_STATUS_OK : PXA_STATUS_NOT_FOUND;
}

static pxa_status_t audio_flush(void *context, uint64_t provider_session) {
    pxa_esp_audio_state_t *audio = (pxa_esp_audio_state_t *)context;
    pxa_esp_audio_sink_t sink;
    pxa_esp_audio_packet_t packet;
    BaseType_t sent;
    int slot_index;
    uint8_t asset_active = 0;
    if (audio != &g_audio) return PXA_STATUS_INVALID_ARGUMENT;
    if (!sink_snapshot(audio, &sink) || sink.flush == NULL)
        return PXA_STATUS_UNSUPPORTED;
    if (uxQueueSpacesAvailable(audio->queue) == 0)
        return PXA_STATUS_WOULD_BLOCK;
    memset(&packet, 0, sizeof(packet));
    packet.kind = PXA_ESP_AUDIO_PACKET_FLUSH;
    portENTER_CRITICAL(&audio->backend_lock);
    slot_index = slot_index_locked(audio, provider_session);
    if (slot_index >= 0) {
        audio->slots[slot_index].epoch++;
        audio->slots[slot_index].queued_samples = 0;
        packet.provider_session = provider_session;
        packet.epoch = audio->slots[slot_index].epoch;
        asset_active = audio->slots[slot_index].asset_active;
        audio->slots[slot_index].asset_active = 0;
    }
    portEXIT_CRITICAL(&audio->backend_lock);
    if (slot_index < 0) return PXA_STATUS_NOT_FOUND;
    if (asset_active) {
        pxa_esp_audio_asset_sink_t asset_sink;
        if (asset_sink_snapshot(audio, &asset_sink)) {
            (void)asset_sink.control(asset_sink.context, (uint8_t)slot_index,
                                     PXA_AUDIO_ASSET_STOP, 0);
        }
    }
    sent = xQueueSendToBack(audio->queue, &packet, 0);
    return sent == pdTRUE ? PXA_STATUS_OK : PXA_STATUS_WOULD_BLOCK;
}

static void audio_close(void *context, uint64_t provider_session) {
    pxa_esp_audio_state_t *audio = (pxa_esp_audio_state_t *)context;
    pxa_esp_audio_sink_t sink;
    int slot_index;
    uint8_t asset_active = 0;
    if (audio != &g_audio) return;
    portENTER_CRITICAL(&audio->backend_lock);
    slot_index = slot_index_locked(audio, provider_session);
    if (slot_index >= 0) {
        asset_active = audio->slots[slot_index].asset_active;
        audio->slots[slot_index].active = 0;
        audio->slots[slot_index].closing = 1;
    }
    portEXIT_CRITICAL(&audio->backend_lock);
    if (slot_index >= 0 && asset_active) {
        pxa_esp_audio_asset_sink_t asset_sink;
        if (asset_sink_snapshot(audio, &asset_sink)) {
            (void)asset_sink.control(asset_sink.context, (uint8_t)slot_index,
                                     PXA_AUDIO_ASSET_STOP, 0);
        }
    }
    if (slot_index >= 0 && sink_snapshot(audio, &sink) && sink.flush != NULL) {
        sink.flush(sink.context, (uint8_t)slot_index);
    }
    if (slot_index >= 0) {
        portENTER_CRITICAL(&audio->backend_lock);
        if (audio->slots[slot_index].provider_session == provider_session &&
            audio->slots[slot_index].closing) {
            memset(&audio->slots[slot_index], 0,
                   sizeof(audio->slots[slot_index]));
        }
        portEXIT_CRITICAL(&audio->backend_lock);
    }
}

static int packet_is_active(pxa_esp_audio_state_t *audio,
                            const pxa_esp_audio_packet_t *packet,
                            uint8_t *voice) {
    int slot_index;
    portENTER_CRITICAL(&audio->backend_lock);
    slot_index = slot_index_locked(audio, packet->provider_session);
    if (slot_index >= 0 && audio->slots[slot_index].epoch != packet->epoch) {
        slot_index = -1;
    }
    if (slot_index >= 0) {
        pxa_esp_audio_slot_t *slot = &audio->slots[slot_index];
        slot->queued_samples = slot->queued_samples >= packet->samples
                                   ? slot->queued_samples - packet->samples
                                   : 0;
    }
    portEXIT_CRITICAL(&audio->backend_lock);
    if (slot_index < 0) return 0;
    *voice = (uint8_t)slot_index;
    return 1;
}

static void apply_gain(pxa_esp_audio_packet_t *packet) {
    size_t index;
    if (packet->linear_gain == 1.0f) return;
    for (index = 0; index < packet->samples; ++index) {
        const int32_t scaled =
            (int32_t)(packet->pcm[index] * packet->linear_gain);
        packet->pcm[index] = scaled > INT16_MAX ? INT16_MAX
                           : scaled < INT16_MIN ? INT16_MIN
                                                : (int16_t)scaled;
    }
}

static int16_t tone_sample(uint8_t waveform, uint16_t phase,
                           uint32_t *noise_state) {
    const int32_t signed_phase = (int16_t)phase;
    switch (waveform) {
        case PXA_AUDIO_TONE_SQUARE:
            return (phase & UINT16_C(0x8000)) != 0 ? INT16_MAX : INT16_MIN;
        case PXA_AUDIO_TONE_TRIANGLE: {
            const uint16_t ramp = (phase & UINT16_C(0x8000)) != 0
                                      ? (uint16_t)(UINT16_MAX - phase)
                                      : phase;
            return (int16_t)(((int32_t)ramp - 16384) * 2);
        }
        case PXA_AUDIO_TONE_NOISE:
            *noise_state = *noise_state * UINT32_C(1664525) +
                           UINT32_C(1013904223);
            return (int16_t)(*noise_state >> 16);
        default: {
            const uint32_t magnitude =
                (uint32_t)(signed_phase < 0 ? -(int64_t)signed_phase
                                            : signed_phase);
            return (int16_t)((int64_t)4 * signed_phase *
                             (32768u - magnitude) / 32768);
        }
    }
}

static uint32_t render_tone(pxa_esp_audio_packet_t *packet,
                            const pxa_esp_audio_sink_t *sink,
                            uint8_t voice) {
    const uint32_t phase_step =
        (uint32_t)packet->frequency_hz * UINT32_C(65536) /
        PXA_ESP_AUDIO_SAMPLE_RATE;
    const uint32_t total_samples = packet->samples;
    uint32_t accepted_samples = 0;
    uint32_t noise_state = UINT32_C(0x9e3779b9);
    uint32_t generated = 0;
    uint16_t phase = 0;
    while (generated < total_samples) {
        const uint16_t count =
            total_samples - generated > PXA_ESP_AUDIO_FRAME_SAMPLES
                ? PXA_ESP_AUDIO_FRAME_SAMPLES
                : (uint16_t)(total_samples - generated);
        uint16_t index;
        packet->samples = count;
        for (index = 0; index < count; ++index) {
            const uint32_t position = generated + index;
            uint32_t tone_position;
            uint32_t envelope = 256;
            uint32_t remaining;
            if (position < packet->delay_samples) {
                packet->pcm[index] = 0;
                continue;
            }
            tone_position = position - packet->delay_samples;
            remaining = packet->tone_samples - tone_position;
            if (packet->attack_samples != 0 &&
                tone_position < packet->attack_samples) {
                envelope = tone_position * 256u / packet->attack_samples;
            }
            if (packet->release_samples != 0 &&
                remaining <= packet->release_samples) {
                const uint32_t release_envelope =
                    remaining * 256u / packet->release_samples;
                if (release_envelope < envelope)
                    envelope = release_envelope;
            }
            packet->pcm[index] = (int16_t)(
                (int32_t)tone_sample(packet->waveform, phase, &noise_state) *
                (int32_t)envelope / 256);
            phase = (uint16_t)(phase + phase_step);
        }
        apply_gain(packet);
        if (!sink->submit(sink->context, voice, packet->pcm, count)) break;
        accepted_samples += count;
        generated += count;
    }
    return accepted_samples;
}

static void audio_output_task(void *argument) {
    pxa_esp_audio_state_t *audio = (pxa_esp_audio_state_t *)argument;
    for (;;) {
        pxa_esp_audio_packet_t packet;
        pxa_esp_audio_sink_t sink;
        uint8_t voice;
        int accepted;
        if (xQueueReceive(audio->queue, &packet, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (!packet_is_active(audio, &packet, &voice) ||
            !sink_snapshot(audio, &sink)) {
            portENTER_CRITICAL(&audio->backend_lock);
            audio->stale_frames++;
            portEXIT_CRITICAL(&audio->backend_lock);
            continue;
        }
        if (packet.kind == PXA_ESP_AUDIO_PACKET_FLUSH) {
            if (sink.flush != NULL) sink.flush(sink.context, voice);
            continue;
        }
        if (packet.kind == PXA_ESP_AUDIO_PACKET_TONE) {
            const uint32_t requested_samples = packet.samples;
            const uint32_t accepted_samples =
                render_tone(&packet, &sink, voice);
            portENTER_CRITICAL(&audio->backend_lock);
            {
                const int slot_index = slot_index_locked(
                    audio, packet.provider_session);
                if (slot_index >= 0 &&
                    audio->slots[slot_index].epoch == packet.epoch) {
                    audio->slots[slot_index].accepted_samples +=
                        accepted_samples;
                }
                audio->tone_frames +=
                    (accepted_samples + PXA_ESP_AUDIO_FRAME_SAMPLES - 1u) /
                    PXA_ESP_AUDIO_FRAME_SAMPLES;
                if (accepted_samples == requested_samples)
                    ++audio->rendered_frames;
                else
                    ++audio->sink_rejected_frames;
            }
            portEXIT_CRITICAL(&audio->backend_lock);
            continue;
        }
        apply_gain(&packet);
        accepted = sink.submit(sink.context, voice, packet.pcm, packet.samples);
        portENTER_CRITICAL(&audio->backend_lock);
        if (accepted) {
            const int slot_index = slot_index_locked(
                audio, packet.provider_session);
            if (slot_index >= 0 &&
                audio->slots[slot_index].epoch == packet.epoch) {
                audio->slots[slot_index].accepted_samples += packet.samples;
            }
            audio->rendered_frames++;
        } else {
            audio->sink_rejected_frames++;
        }
        portEXIT_CRITICAL(&audio->backend_lock);
    }
}

int pxa_esp_audio_initialize(void) {
    if (g_audio.queue != NULL && g_audio.task != NULL) return 1;
    if (g_audio.queue != NULL || g_audio.task != NULL) {
        pxa_esp_audio_deinitialize();
    }
    g_audio.queue = xQueueCreateWithCaps(
        PXA_ESP_AUDIO_QUEUE_LENGTH, sizeof(pxa_esp_audio_packet_t),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (g_audio.queue == NULL) return 0;
    if (xTaskCreateWithCaps(audio_output_task, "pxa_audio",
                            PXA_ESP_AUDIO_TASK_STACK_SIZE, &g_audio,
                            PXA_ESP_AUDIO_TASK_PRIORITY, &g_audio.task,
                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
        vQueueDelete(g_audio.queue);
        g_audio.queue = NULL;
        return 0;
    }
    return 1;
}

void pxa_esp_audio_deinitialize(void) {
    if (g_audio.task != NULL) {
        vTaskDelete(g_audio.task);
        g_audio.task = NULL;
    }
    if (g_audio.queue != NULL) {
        vQueueDelete(g_audio.queue);
        g_audio.queue = NULL;
    }
    pxa_esp_audio_reset_sessions();
}

void pxa_esp_audio_reset_sessions(void) {
    pxa_esp_audio_sink_t sink;
    uint16_t active_mask = 0;
    uint16_t asset_mask = 0;
    uint16_t index;
    portENTER_CRITICAL(&g_audio.backend_lock);
    for (index = 0; index < PXA_ESP_AUDIO_VOICE_COUNT; ++index) {
        if (g_audio.slots[index].active || g_audio.slots[index].closing) {
            active_mask |= (uint16_t)(UINT16_C(1) << index);
        }
        if (g_audio.slots[index].asset_active)
            asset_mask |= (uint16_t)(UINT16_C(1) << index);
    }
    memset(g_audio.slots, 0, sizeof(g_audio.slots));
    g_audio.package_manifest = NULL;
    g_audio.package_root[0] = '\0';
    portEXIT_CRITICAL(&g_audio.backend_lock);
    {
        pxa_esp_audio_asset_sink_t asset_sink;
        if (asset_sink_snapshot(&g_audio, &asset_sink)) {
            for (index = 0; index < PXA_ESP_AUDIO_VOICE_COUNT; ++index) {
                if ((asset_mask & (uint16_t)(UINT16_C(1) << index)) != 0) {
                    (void)asset_sink.control(asset_sink.context,
                                             (uint8_t)index,
                                             PXA_AUDIO_ASSET_STOP, 0);
                }
            }
        }
    }
    if (!sink_snapshot(&g_audio, &sink) || sink.flush == NULL) return;
    for (index = 0; index < PXA_ESP_AUDIO_VOICE_COUNT; ++index) {
        if ((active_mask & (uint16_t)(UINT16_C(1) << index)) != 0) {
            sink.flush(sink.context, (uint8_t)index);
        }
    }
}

void pxa_esp_audio_backend(pxa_audio_backend_t *output) {
    if (output == NULL) return;
    memset(output, 0, sizeof(*output));
    output->struct_size = sizeof(*output);
    output->context = &g_audio;
    output->open = audio_open;
    output->commit = audio_commit;
    output->submit = audio_submit;
    output->query = audio_query;
    output->flush = audio_flush;
    output->play_tone = audio_play_tone;
    output->play_asset = audio_play_asset;
    output->control_asset = audio_control_asset;
    output->close = audio_close;
}

void pxa_esp_audio_snapshot(pxa_esp_audio_snapshot_t *output) {
    uint16_t index;
    if (output == NULL) return;
    memset(output, 0, sizeof(*output));
    if (g_audio.queue != NULL) {
        output->queue_capacity = PXA_ESP_AUDIO_QUEUE_LENGTH;
        output->queue_storage_bytes =
            PXA_ESP_AUDIO_QUEUE_LENGTH * sizeof(pxa_esp_audio_packet_t);
        output->queued_frames = (uint32_t)uxQueueMessagesWaiting(g_audio.queue);
    }
    if (g_audio.task != NULL) output->task_stack_bytes = PXA_ESP_AUDIO_TASK_STACK_SIZE;
    portENTER_CRITICAL(&g_audio.backend_lock);
    output->submitted_frames = g_audio.submitted_frames;
    output->rendered_frames = g_audio.rendered_frames;
    output->queue_full_frames = g_audio.queue_full_frames;
    output->stale_frames = g_audio.stale_frames;
    output->sink_rejected_frames = g_audio.sink_rejected_frames;
    output->tone_commands = g_audio.tone_commands;
    output->tone_frames = g_audio.tone_frames;
    output->tone_dropped_commands = g_audio.tone_dropped_commands;
    output->voice_exhaustions = g_audio.voice_exhaustions;
    output->asset_play_commands = g_audio.asset_play_commands;
    output->asset_control_commands = g_audio.asset_control_commands;
    output->asset_command_failures = g_audio.asset_command_failures;
    output->peak_queued_frames = g_audio.peak_queued_frames;
    for (index = 0; index < PXA_ESP_AUDIO_VOICE_COUNT; ++index) {
        if (g_audio.slots[index].active) output->active_sessions++;
    }
    portEXIT_CRITICAL(&g_audio.backend_lock);
}

int pxa_esp_audio_bind_package(const pxa_package_manifest_t *manifest,
                               const char *package_root) {
    size_t root_size;
    if (manifest == NULL || package_root == NULL) return 0;
    root_size = bounded_string_size(package_root,
                                    PXA_ESP_AUDIO_ASSET_PATH_MAX);
    if (root_size == 0 || root_size >= PXA_ESP_AUDIO_ASSET_PATH_MAX) return 0;
    portENTER_CRITICAL(&g_audio.backend_lock);
    g_audio.package_manifest = manifest;
    memcpy(g_audio.package_root, package_root, root_size + 1u);
    portEXIT_CRITICAL(&g_audio.backend_lock);
    return 1;
}

void pxa_esp_audio_set_sink(pxa_host_audio_submit_fn submit,
                            pxa_host_audio_flush_fn flush, void *context) {
    portENTER_CRITICAL(&g_audio.sink_lock);
    g_audio.sink.submit = submit;
    g_audio.sink.flush = flush;
    g_audio.sink.context = context;
    portEXIT_CRITICAL(&g_audio.sink_lock);
}

void pxa_esp_audio_set_asset_sink(
    pxa_host_audio_asset_play_fn play,
    pxa_host_audio_asset_control_fn control, void *context) {
    portENTER_CRITICAL(&g_audio.sink_lock);
    g_audio.asset_sink.play = play;
    g_audio.asset_sink.control = control;
    g_audio.asset_sink.context = context;
    portEXIT_CRITICAL(&g_audio.sink_lock);
}

#endif
