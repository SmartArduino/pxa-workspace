#include "pxa_esp_audio.h"

#if defined(ESP_PLATFORM)

#include <limits.h>
#include <math.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#if PXA_AUDIO_LATENCY_TRACE
#include "esp_timer.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "pxa/audio_mixer.h"
#include "pxa_esp_resource_memory.h"
#include "freertos/task.h"
#if PXA_HAS_ENCODED_AUDIO
#include "pxa_esp_encoded_audio.h"
#endif

#define PXA_ESP_AUDIO_TAG "PxaAudio"
#define PXA_ESP_AUDIO_SAMPLE_RATE 16000
#define PXA_ESP_AUDIO_FRAME_MS 20
#define PXA_ESP_AUDIO_FRAME_SAMPLES \
    (PXA_ESP_AUDIO_SAMPLE_RATE * PXA_ESP_AUDIO_FRAME_MS / 1000)
#define PXA_ESP_AUDIO_TASK_STACK_SIZE 4096
#define PXA_ESP_AUDIO_TASK_PRIORITY 5

typedef struct {
    uint64_t provider_session;
    float linear_gain;
    uint8_t active;
    uint8_t graph_committed;
    uint8_t closing;
    uint8_t asset_active;
    uint8_t asset_guest_paused;
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
    pxa_audio_mixer_voice_t *voices[PXA_ESP_AUDIO_VOICE_COUNT];
    bool initialized, deinitializing;
    SemaphoreHandle_t mutex;
    StaticSemaphore_t mutex_storage;
    TaskHandle_t task; /* Legacy push adapter only; board output uses pull. */
    uint32_t output_rate;
    void (*device_wake)(void *);
    void *device_context;
    portMUX_TYPE backend_lock;
    portMUX_TYPE sink_lock;
    pxa_esp_audio_slot_t slots[PXA_ESP_AUDIO_VOICE_COUNT];
    pxa_esp_audio_sink_t sink;
    pxa_esp_audio_asset_sink_t asset_sink;
    pxa_host_audio_music_sink_t music_sink;
    pxa_host_audio_sound_fn play_sound;
    void *sound_context;
    pxa_host_audio_sound_track_fn play_sound_track;
    pxa_host_audio_sound_control_fn control_sound;
    void *sound_track_context;
    const pxa_package_manifest_t *package_manifest;
    char package_root[PXA_ESP_AUDIO_ASSET_PATH_MAX];
#if PXA_HAS_ENCODED_AUDIO
    uint8_t encoded_wait_slot; /* Mixer lock protects the one-based index and clock. */
    uint64_t encoded_wake_sample;
#endif
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
    uint8_t suspended;
} pxa_esp_audio_state_t;

static pxa_esp_audio_state_t g_audio = {
    .backend_lock = portMUX_INITIALIZER_UNLOCKED,
    .sink_lock = portMUX_INITIALIZER_UNLOCKED,
};

/* Creation uses only static storage. Rendering and sink submission are protected
 * by a sleeping mutex, never an interrupt-disabling critical section. */
static void audio_lock(pxa_esp_audio_state_t *audio) {
    portENTER_CRITICAL(&audio->backend_lock);
    if (audio->mutex == NULL)
        audio->mutex = xSemaphoreCreateMutexStatic(&audio->mutex_storage);
    portEXIT_CRITICAL(&audio->backend_lock);
    xSemaphoreTake(audio->mutex, portMAX_DELAY);
}
static void audio_unlock(pxa_esp_audio_state_t *audio) {
    xSemaphoreGive(audio->mutex);
}

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

static int asset_path_is_audio(const uint8_t *path, size_t size) {
    return path != NULL && size > 4u &&
           memcmp(path + size - 4u, ".ogg", 4u) == 0;
}

static bool initialize_locked(void);

static pxa_status_t audio_open_format(void *context, const pxa_audio_format_t *format,
    pxa_audio_pcm_info_t *info, uint64_t *provider_session, bool buffered) {
    pxa_esp_audio_state_t *audio = (pxa_esp_audio_state_t *)context;
    pxa_esp_audio_sink_t sink;
    if (audio != &g_audio || !format || !provider_session) return PXA_STATUS_UNSUPPORTED;
    audio_lock(audio);
    if ((!sink_snapshot(audio, &sink) && !audio->device_wake) || (info && !audio->device_wake)) {
        audio_unlock(audio); return PXA_STATUS_UNSUPPORTED;
    }
    if (!initialize_locked()) { audio_unlock(audio); return PXA_STATUS_RESOURCE_LIMIT; }
    unsigned index = 0;
    while (index < PXA_ESP_AUDIO_VOICE_COUNT && audio->slots[index].provider_session) ++index;
    if (index == PXA_ESP_AUDIO_VOICE_COUNT) {
        ++audio->voice_exhaustions;
        audio_unlock(audio); return PXA_STATUS_RESOURCE_LIMIT;
    }
    const bool pull = audio->device_wake != NULL;
    const uint32_t output_rate = pull ? audio->output_rate : PXA_ESP_AUDIO_SAMPLE_RATE;
    uint64_t session = ++audio->next_provider_session;
    if (!session) session = ++audio->next_provider_session;
    pxa_esp_audio_slot_t *slot = &audio->slots[index];
    /* Reserve this generation. A reset can cancel it before publication. */
    slot->provider_session = session;
    audio_unlock(audio);

    pxa_audio_mixer_voice_t *voice = pxa_memory_allocate(
        pxa_esp_device_resource_allocator(PXA_MEMORY_EXTERNAL, PXA_MEMORY_AUDIO), sizeof(*voice));
    pxa_status_t status = PXA_STATUS_RESOURCE_LIMIT;
    if (voice) {
        pxa_audio_mixer_reset(voice);
        status = pxa_audio_mixer_configure(voice, format->sample_rate, format->channels, output_rate);
        if (!status && buffered) status = pxa_audio_mixer_buffered(voice);
        if (pull) voice->state.flags = 0;
    }
    audio_lock(audio);
    if (slot->provider_session != session) {
        status = PXA_STATUS_UNAVAILABLE;
    } else if (status || !audio->initialized) {
        slot->provider_session = 0;
        if (!status) status = PXA_STATUS_UNAVAILABLE;
    } else {
        audio->voices[index] = voice;
        slot->active = 1;
        *provider_session = session;
        if (info) *info = (pxa_audio_pcm_info_t){output_rate,
            voice->queue_limit * (format->sample_rate / 50u), 1};
    }
    audio_unlock(audio);
    if (status) pxa_memory_release(voice);
    return status;
}

static pxa_status_t audio_open(void *context, uint16_t usage,
    pxa_audio_format_t *format, uint64_t *session) {
    if (!format || usage != PXA_AUDIO_USAGE_MEDIA) return PXA_STATUS_UNSUPPORTED;
    *format = (pxa_audio_format_t){PXA_ESP_AUDIO_SAMPLE_RATE, 1, PXA_ESP_AUDIO_FRAME_MS};
    return audio_open_format(context, format, NULL, session, false);
}
static pxa_status_t open_pcm(void *context, const pxa_audio_format_t *format,
    pxa_audio_pcm_info_t *info, uint64_t *session, bool buffered) {
    if (!format || !info || format->frame_ms != 20 || format->sample_rate < 8000 ||
        format->sample_rate > 48000 || (format->channels != 1 && format->channels != 2))
        return PXA_STATUS_INVALID_ARGUMENT;
    return audio_open_format(context, format, info, session, buffered);
}
static pxa_status_t audio_open_pcm(void *context, const pxa_audio_format_t *format,
    pxa_audio_pcm_info_t *info, uint64_t *session) {
    return open_pcm(context, format, info, session, false);
}
static pxa_status_t audio_open_pcm_buffered(void *context, const pxa_audio_format_t *format,
    pxa_audio_pcm_info_t *info, uint64_t *session) {
    if (PXA_AUDIO_MIXER_PACKETS < 24) return PXA_STATUS_UNSUPPORTED;
    return open_pcm(context, format, info, session, true);
}
static void audio_unlock_and_wake(pxa_esp_audio_state_t *audio, bool wake) {
    void (*device_wake)(void *) = wake ? audio->device_wake : NULL;
    void *context = audio->device_context;
    /* The legacy output task can be deleted as soon as we unlock. */
    if (wake && !device_wake && audio->task) xTaskNotifyGive(audio->task);
    audio_unlock(audio);
    if (device_wake) device_wake(context);
}
static pxa_status_t audio_commit(void *context, uint64_t session,
                                  const pxa_audio_graph_t *graph) {
    pxa_esp_audio_state_t *audio = context;
    if (audio != &g_audio || !graph) return PXA_STATUS_INVALID_ARGUMENT;
    audio_lock(audio);
    int index = slot_index_locked(audio, session);
    pxa_status_t status = index < 0 ? PXA_STATUS_NOT_FOUND :
        pxa_audio_mixer_commit(audio->voices[index], graph);
    if (status == PXA_STATUS_OK) {
        audio->slots[index].graph_committed = 1;
        audio->slots[index].linear_gain = audio->voices[index]->gain;
    }
    audio_unlock(audio);
    return status;
}

static void update_queue_peak(pxa_esp_audio_state_t *audio) {
    uint32_t count = 0;
    for (unsigned i = 0; i < PXA_AUDIO_MIXER_VOICES; ++i)
        if (audio->voices[i]) count += audio->voices[i]->count;
    if (count > audio->peak_queued_frames) audio->peak_queued_frames = count;
}

static pxa_status_t submit_pcm(void *context, uint64_t session, const uint8_t *pcm,
                               size_t size, bool encoded) {
    pxa_esp_audio_state_t *audio = context;
    if (audio != &g_audio) return PXA_STATUS_INVALID_ARGUMENT;
    audio_lock(audio);
    int index = slot_index_locked(audio, session);
    pxa_status_t status = index < 0 ? PXA_STATUS_NOT_FOUND :
        pxa_audio_mixer_write(audio->voices[index], pcm, size);
    if (status == PXA_STATUS_OK) { ++audio->submitted_frames; update_queue_peak(audio); }
    if (status == PXA_STATUS_WOULD_BLOCK) ++audio->queue_full_frames;
#if PXA_HAS_ENCODED_AUDIO
    if (encoded && index >= 0) {
        if (status == PXA_STATUS_WOULD_BLOCK) {
            const pxa_audio_mixer_voice_t *voice = audio->voices[index];
            /* With a deep queue, release two 20ms packets before waking AAC. */
            uint32_t frames = voice->source_rate / 50;
            if (voice->state.queued_samples >= 4 * frames) frames *= 2;
            if (frames > voice->state.queued_samples) frames = voice->state.queued_samples;
            audio->encoded_wait_slot = (uint8_t)(index + 1);
            audio->encoded_wake_sample = voice->state.accepted_samples + frames;
        } else {
            audio->encoded_wait_slot = 0;
        }
    }
#else
    (void)encoded;
#endif
    audio_unlock_and_wake(audio, status == PXA_STATUS_OK);
    return status;
}

static pxa_status_t audio_submit(void *context, uint64_t session, const uint8_t *pcm, size_t size) {
    return submit_pcm(context, session, pcm, size, false);
}
#if PXA_HAS_ENCODED_AUDIO
static pxa_status_t submit_encoded_pcm(void *context, uint64_t session, const uint8_t *pcm, size_t size) {
    return submit_pcm(context, session, pcm, size, true);
}
static bool encoded_can_resume_locked(pxa_esp_audio_state_t *audio) {
    if (!audio->encoded_wait_slot) return false;
    const pxa_audio_mixer_voice_t *voice = audio->voices[audio->encoded_wait_slot - 1];
    if (voice->count && voice->state.accepted_samples < audio->encoded_wake_sample) return false;
    audio->encoded_wait_slot = 0;
    return true;
}
static int32_t audio_encoded_io(void *context, uint64_t session,
    const pxa_audio_format_t *format, uint32_t operation, uint8_t *data, size_t size) {
    if (operation == PXA_AUDIO_IO_CONFIGURE_ENCODED) {
        pxa_esp_audio_state_t *audio = context;
        audio_lock(audio);
        int index = slot_index_locked(audio, session);
        int ready = index >= 0 && audio->slots[index].graph_committed &&
            !audio->voices[index]->state.queued_samples;
        audio_unlock(audio);
        if (!ready) return PXA_STATUS_BAD_STATE;
    }
    return pxa_esp_encoded_audio_io(context, session, format, operation, data, size, submit_encoded_pcm);
}
#endif

static pxa_status_t audio_play_tone(void *context, uint64_t session,
                                     const pxa_audio_tone_t *tone) {
    pxa_esp_audio_state_t *audio = context;
    if (audio != &g_audio) return PXA_STATUS_INVALID_ARGUMENT;
    audio_lock(audio);
    int index = slot_index_locked(audio, session);
    pxa_status_t status = index < 0 ? PXA_STATUS_NOT_FOUND :
        pxa_audio_mixer_tone(audio->voices[index], tone);
    if (status == PXA_STATUS_OK) {
        ++audio->submitted_frames;
        ++audio->tone_commands;
#if PXA_AUDIO_LATENCY_TRACE
        unsigned packet = audio->voices[index]->read;
        while (audio->voices[index]->packets[packet].next != PXA_AUDIO_MIXER_PACKETS)
            packet = audio->voices[index]->packets[packet].next;
        esp_log_write(ESP_LOG_INFO, "AudioTrace", "AudioTrace: tone us=%lld session=%llu packet=%u duration=%u delay=%u queued=%u\n",
                 (long long)esp_timer_get_time(), (unsigned long long)session,
                 packet,
                 tone->duration_ms, tone->delay_ms,
                 (unsigned)audio->voices[index]->state.queued_samples);
#endif
        update_queue_peak(audio);
    }
    if (status == PXA_STATUS_WOULD_BLOCK) {
#if PXA_AUDIO_LATENCY_TRACE
        esp_log_write(ESP_LOG_INFO, "AudioTrace", "AudioTrace: tone_rejected us=%lld session=%llu duration=%u delay=%u\n",
                 (long long)esp_timer_get_time(), (unsigned long long)session,
                 tone->duration_ms, tone->delay_ms);
#endif
        ++audio->queue_full_frames;
        ++audio->tone_dropped_commands;
    }
    audio_unlock_and_wake(audio, status == PXA_STATUS_OK);
    return status;
}

static pxa_status_t audio_play_effect(void *context, uint64_t session,
    const pxa_audio_tone_t *tones, size_t count, uint8_t track) {
    pxa_esp_audio_state_t *audio = context;
    if (audio != &g_audio) return PXA_STATUS_INVALID_ARGUMENT;
    audio_lock(audio);
    int index = slot_index_locked(audio, session);
#if PXA_AUDIO_LATENCY_TRACE
    /* Identify the published slot without guessing FIFO placement. A fresh
     * independent group consumes a previously free slot; named replacement
     * has a unique track even if another group has identical layers. */
    uint8_t occupied = 0;
    if (index >= 0) for (unsigned slot = 0; slot < PXA_AUDIO_MIXER_PACKETS; ++slot) {
        const pxa_audio_mixer_packet_t *p = &audio->voices[index]->packets[slot];
        if (p->position < p->length) occupied |= 1u << slot;
    }
#endif
    pxa_status_t status = index < 0 ? PXA_STATUS_NOT_FOUND :
        pxa_audio_mixer_effect(audio->voices[index], tones, count, track);
    if (status == PXA_STATUS_OK) {
        ++audio->submitted_frames;
        audio->tone_commands += count;
        update_queue_peak(audio);
#if PXA_AUDIO_LATENCY_TRACE
        const pxa_audio_mixer_voice_t *v = audio->voices[index];
        for (unsigned slot = 0; slot < PXA_AUDIO_MIXER_PACKETS; ++slot) {
            const pxa_audio_mixer_packet_t *p = &v->packets[slot];
            if (p->kind == PXA_AUDIO_PACKET_EFFECT && p->position == 0 &&
                (!(occupied & (1u << slot)) ||
                 (track != PXA_AUDIO_SOUND_APPEND && p->meta.effect.track == track))) {
                esp_log_write(ESP_LOG_INFO, "AudioTrace", "AudioTrace: effect us=%lld session=%llu packet=%u layers=%u track=%u queued=%u\n",
                    (long long)esp_timer_get_time(), (unsigned long long)session, slot,
                    (unsigned)count, track, (unsigned)v->state.queued_samples);
                break;
            }
        }
#endif
    }
    if (status == PXA_STATUS_WOULD_BLOCK) {
#if PXA_AUDIO_LATENCY_TRACE
        esp_log_write(ESP_LOG_INFO, "AudioTrace", "AudioTrace: effect_rejected us=%lld session=%llu layers=%u track=%u\n",
            (long long)esp_timer_get_time(), (unsigned long long)session, (unsigned)count, track);
#endif
        ++audio->queue_full_frames;
        audio->tone_dropped_commands += count;
    }
    audio_unlock_and_wake(audio, status == PXA_STATUS_OK);
    return status;
}
static pxa_status_t audio_stop_effect(void *context, uint64_t session, uint8_t track) {
    pxa_esp_audio_state_t *audio = context;
    if (audio != &g_audio) return PXA_STATUS_INVALID_ARGUMENT;
    audio_lock(audio);
    int index = slot_index_locked(audio, session);
    pxa_status_t status = index < 0 ? PXA_STATUS_NOT_FOUND :
        pxa_audio_mixer_stop_effect(audio->voices[index], track);
    audio_unlock(audio);
    return status;
}

static pxa_status_t audio_play_sound(void *context,uint64_t session,pxa_asset_object_t *sound,int16_t gain) {
    pxa_esp_audio_state_t *audio = context;
    pxa_host_audio_sound_fn play; void *sink_context;
    if (audio != &g_audio || !sound) return PXA_STATUS_INVALID_ARGUMENT;
    portENTER_CRITICAL(&audio->sink_lock);
    play = audio->play_sound; sink_context = audio->sound_context;
    portEXIT_CRITICAL(&audio->sink_lock);
    if (!play) return PXA_STATUS_UNSUPPORTED;
    audio_lock(audio);
    int slot = slot_index_locked(audio,session);
    int committed = slot >= 0 && audio->slots[slot].graph_committed;
    int suspended = audio->suspended;
    audio_unlock(audio);
    if (slot < 0) return PXA_STATUS_NOT_FOUND;
    if (!committed) return PXA_STATUS_BAD_STATE;
    if (!play(sink_context,(uint8_t)slot,sound,gain)) return PXA_STATUS_WOULD_BLOCK;
    audio_lock(audio);
    audio->slots[slot].asset_active = 1; audio->slots[slot].asset_guest_paused = 0;
    audio_unlock(audio);
    if (suspended) {
        pxa_esp_audio_asset_sink_t sink;
        if (asset_sink_snapshot(audio,&sink))
            (void)sink.control(sink.context,(uint8_t)slot,PXA_AUDIO_ASSET_PAUSE,0);
    }
    return PXA_STATUS_OK;
}

static pxa_status_t audio_play_sound_ex(void *context, uint64_t session,
    pxa_asset_object_t *sound, const pxa_audio_sound_options_t *options) {
    pxa_esp_audio_state_t *audio = context;
    if (audio != &g_audio || !sound || !options) return PXA_STATUS_INVALID_ARGUMENT;
    audio_lock(audio);
    int slot = slot_index_locked(audio,session);
    int committed = slot >= 0 && audio->slots[slot].graph_committed;
    int suspended = audio->suspended;
    audio_unlock(audio);
    if (slot < 0) return PXA_STATUS_NOT_FOUND;
    if (!committed) return PXA_STATUS_BAD_STATE;
    if (suspended) return PXA_STATUS_UNAVAILABLE;
    portENTER_CRITICAL(&audio->sink_lock);
    pxa_host_audio_sound_track_fn play = audio->play_sound_track;
    void *sink_context = audio->sound_track_context;
    portEXIT_CRITICAL(&audio->sink_lock);
    if (!play) return PXA_STATUS_UNSUPPORTED;
    if (!play(sink_context,(uint8_t)slot,sound,options)) return PXA_STATUS_WOULD_BLOCK;
    audio_lock(audio);
    audio->slots[slot].asset_active = 1;
    audio_unlock(audio);
    return PXA_STATUS_OK;
}
static pxa_status_t audio_control_sound(void *context, uint64_t session,
    const pxa_audio_sound_control_t *control) {
    pxa_esp_audio_state_t *audio = context;
    if (audio != &g_audio || !control) return PXA_STATUS_INVALID_ARGUMENT;
    audio_lock(audio);
    int slot = slot_index_locked(audio,session);
    audio_unlock(audio);
    if (slot < 0) return PXA_STATUS_NOT_FOUND;
    portENTER_CRITICAL(&audio->sink_lock);
    pxa_host_audio_sound_control_fn apply = audio->control_sound;
    void *sink_context = audio->sound_track_context;
    portEXIT_CRITICAL(&audio->sink_lock);
    if (!apply) return PXA_STATUS_UNSUPPORTED;
    return apply(sink_context,(uint8_t)slot,control) ? PXA_STATUS_OK : PXA_STATUS_BAD_STATE;
}

static pxa_status_t audio_play_music_impl(void *context,
                                     uint64_t provider_session,
                                     const pxa_audio_asset_t *asset, uint64_t *instance) {
    pxa_esp_audio_state_t *audio = (pxa_esp_audio_state_t *)context;
    if (audio!=&g_audio) return PXA_STATUS_INVALID_ARGUMENT;
    pxa_esp_audio_asset_sink_t sink;
    const pxa_package_manifest_t *manifest;
    const pxa_package_file_t *file = NULL;
    char absolute_path[PXA_ESP_AUDIO_ASSET_PATH_MAX];
    size_t root_size;
    int slot_index;
    int accepted;
    int pause_after_play = 0;
    pxa_host_audio_music_sink_t music;
    portENTER_CRITICAL(&audio->sink_lock);
    music=audio->music_sink;
    portEXIT_CRITICAL(&audio->sink_lock);
    if (instance) *instance=0;
    if (instance && !music.play) return PXA_STATUS_UNSUPPORTED;
    if (audio != &g_audio || asset == NULL || asset->path == NULL ||
        asset->path_size == 0 || !asset_path_is_audio(asset->path,
                                                    asset->path_size) ||
        !asset_sink_snapshot(audio, &sink)) {
        return PXA_STATUS_UNSUPPORTED;
    }
    audio_lock(audio);
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
    audio_unlock(audio);
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
    audio_lock(audio);
    if (audio->package_manifest != manifest ||
        slot_index_locked(audio, provider_session) != slot_index) {
        slot_index = -1;
    }
    audio_unlock(audio);
    if (slot_index < 0) return PXA_STATUS_NOT_FOUND;
    pxa_status_t play_status=PXA_STATUS_OK;
    if (music.play) {
        uint64_t ignored_instance;
        audio_lock(audio);
        int suspended=audio->suspended;
        audio_unlock(audio);
        play_status=music.play(music.context,(uint8_t)slot_index,provider_session,
            absolute_path,(asset->flags & PXA_AUDIO_ASSET_LOOP)!=0,asset->gain_db_q8,
            suspended,instance ? instance : &ignored_instance);
        accepted=play_status==PXA_STATUS_OK;
    } else {
        accepted = sink.play(sink.context, (uint8_t)slot_index, absolute_path,
                             (asset->flags & PXA_AUDIO_ASSET_LOOP) != 0,asset->gain_db_q8);
        if (!accepted) play_status=PXA_STATUS_WOULD_BLOCK;
    }
    audio_lock(audio);
    ++audio->asset_play_commands;
    if (accepted && slot_index_locked(audio, provider_session) == slot_index) {
        audio->slots[slot_index].asset_active = 1;
        audio->slots[slot_index].asset_guest_paused = 0;
        pause_after_play = audio->suspended;
    } else if (!accepted) {
        ++audio->asset_command_failures;
    }
    audio_unlock(audio);
    if (pause_after_play)
        (void)sink.control(sink.context, (uint8_t)slot_index,
                           PXA_AUDIO_ASSET_PAUSE, 0);
    return play_status;
}

static pxa_status_t audio_play_asset(void *c,uint64_t s,const pxa_audio_asset_t *a) {
    return audio_play_music_impl(c,s,a,NULL);
}
static pxa_status_t audio_play_music(void *c,uint64_t s,const pxa_audio_asset_t *a,uint64_t *id) {
    if (!id) return PXA_STATUS_INVALID_ARGUMENT;
    return audio_play_music_impl(c,s,a,id);
}
static pxa_status_t audio_playback_peek(void *c,pxa_audio_playback_event_t *event) {
    pxa_esp_audio_state_t *audio=c;
    portENTER_CRITICAL(&audio->sink_lock);
    pxa_host_audio_music_sink_t sink=audio->music_sink;
    portEXIT_CRITICAL(&audio->sink_lock);
    return sink.peek ? sink.peek(sink.context,event) : PXA_STATUS_NOT_FOUND;
}
static pxa_status_t audio_playback_consume(void *c,const pxa_audio_playback_event_t *event) {
    pxa_esp_audio_state_t *audio=c;
    portENTER_CRITICAL(&audio->sink_lock);
    pxa_host_audio_music_sink_t sink=audio->music_sink;
    portEXIT_CRITICAL(&audio->sink_lock);
    return sink.consume ? sink.consume(sink.context,event) : PXA_STATUS_NOT_FOUND;
}
void pxa_esp_audio_set_music_sink(const pxa_host_audio_music_sink_t *sink) {
    portENTER_CRITICAL(&g_audio.sink_lock);
    if (sink) g_audio.music_sink=*sink;
    else memset(&g_audio.music_sink,0,sizeof(g_audio.music_sink));
    portEXIT_CRITICAL(&g_audio.sink_lock);
}

static pxa_status_t audio_control_music(void *context,uint64_t session,
    const pxa_audio_asset_control_t *control) {
    pxa_esp_audio_state_t *audio=context;
    audio_lock(audio);
    int slot=slot_index_locked(audio,session);
    int suspended=audio->suspended;
    audio_unlock(audio);
    if (slot<0) return PXA_STATUS_NOT_FOUND;
    portENTER_CRITICAL(&audio->sink_lock);
    pxa_host_audio_music_sink_t sink=audio->music_sink;
    portEXIT_CRITICAL(&audio->sink_lock);
    if (!sink.control) return PXA_STATUS_UNSUPPORTED;
    if (suspended && control->action==PXA_AUDIO_ASSET_RESUME) return PXA_STATUS_UNAVAILABLE;
    return sink.control(sink.context,(uint8_t)slot,control->action,control->gain_db_q8) ?
        PXA_STATUS_OK : PXA_STATUS_BAD_STATE;
}

static pxa_status_t audio_control_asset(
    void *context, uint64_t provider_session,
    const pxa_audio_asset_control_t *control) {
    pxa_esp_audio_state_t *audio = (pxa_esp_audio_state_t *)context;
    pxa_esp_audio_asset_sink_t sink;
    int slot_index;
    int accepted;
    int suspended;
    if (audio != &g_audio || control == NULL ||
        !asset_sink_snapshot(audio, &sink)) {
        return PXA_STATUS_UNSUPPORTED;
    }
    audio_lock(audio);
    slot_index = slot_index_locked(audio, provider_session);
    if (slot_index >= 0 && !audio->slots[slot_index].asset_active)
        slot_index = -2;
    suspended = audio->suspended;
    audio_unlock(audio);
    if (slot_index == -2) return PXA_STATUS_BAD_STATE;
    if (slot_index < 0) return PXA_STATUS_NOT_FOUND;
    accepted = suspended && control->action == PXA_AUDIO_ASSET_RESUME
                   ? 1
                   : sink.control(sink.context, (uint8_t)slot_index,
                                  control->action, control->gain_db_q8);
    audio_lock(audio);
    ++audio->asset_control_commands;
    if (accepted && control->action == PXA_AUDIO_ASSET_STOP &&
        slot_index_locked(audio, provider_session) == slot_index) {
        audio->slots[slot_index].asset_active = 0;
        audio->slots[slot_index].asset_guest_paused = 0;
    } else if (accepted &&
               slot_index_locked(audio, provider_session) == slot_index) {
        if (control->action == PXA_AUDIO_ASSET_PAUSE)
            audio->slots[slot_index].asset_guest_paused = 1;
        else if (control->action == PXA_AUDIO_ASSET_RESUME)
            audio->slots[slot_index].asset_guest_paused = 0;
    } else if (!accepted) {
        ++audio->asset_command_failures;
    }
    audio_unlock(audio);
    return accepted ? PXA_STATUS_OK : PXA_STATUS_WOULD_BLOCK;
}

static pxa_status_t audio_query(void *context, uint64_t provider_session,
                                pxa_audio_state_t *state) {
    pxa_esp_audio_state_t *audio = (pxa_esp_audio_state_t *)context;
    int slot_index;
    if (audio != &g_audio || state == NULL)
        return PXA_STATUS_INVALID_ARGUMENT;
    audio_lock(audio);
    slot_index = slot_index_locked(audio, provider_session);
    if (slot_index >= 0) {
        *state = audio->voices[slot_index]->state;
    }
    audio_unlock(audio);
    return slot_index >= 0 ? PXA_STATUS_OK : PXA_STATUS_NOT_FOUND;
}

static pxa_status_t audio_flush(void *context, uint64_t session) {
    pxa_esp_audio_state_t *audio = context;
    if (audio != &g_audio) return PXA_STATUS_INVALID_ARGUMENT;
#if PXA_HAS_ENCODED_AUDIO
    pxa_esp_encoded_audio_detach(session);
#endif
    audio_lock(audio);
    int index = slot_index_locked(audio, session);
    if (index >= 0) {
#if PXA_HAS_ENCODED_AUDIO
        if (audio->encoded_wait_slot==index+1) audio->encoded_wait_slot=0;
#endif
        pxa_audio_mixer_flush(audio->voices[index]);
        audio->slots[index].asset_active = 0;
        audio->slots[index].asset_guest_paused = 0;
    }
    audio_unlock(audio);
    if (index < 0) return PXA_STATUS_NOT_FOUND;
    pxa_esp_audio_asset_sink_t sink;
    if (asset_sink_snapshot(audio, &sink))
        (void)sink.control(sink.context, (uint8_t)index, PXA_AUDIO_ASSET_STOP, 0);
    /* Already mixed hardware frames cannot be removed per voice. Do not flush
     * another session's audio; only global suspension flushes the device. */
    return PXA_STATUS_OK;
}

static void audio_close(void *context, uint64_t provider_session) {
    pxa_esp_audio_state_t *audio = (pxa_esp_audio_state_t *)context;
    int slot_index;
    pxa_audio_mixer_voice_t *retired = NULL;
    uint8_t asset_active = 0;
    if (audio != &g_audio) return;
#if PXA_HAS_ENCODED_AUDIO
    pxa_esp_encoded_audio_detach(provider_session);
#endif
    audio_lock(audio);
    slot_index = slot_index_locked(audio, provider_session);
    if (slot_index >= 0) {
        asset_active = audio->slots[slot_index].asset_active;
#if PXA_HAS_ENCODED_AUDIO
        if (audio->encoded_wait_slot==slot_index+1) audio->encoded_wait_slot=0;
#endif
        retired = audio->voices[slot_index];
        audio->voices[slot_index] = NULL;
        audio->slots[slot_index].active = 0;
        audio->slots[slot_index].closing = 1;
    }
    audio_unlock(audio);
    pxa_memory_release(retired);
    if (slot_index >= 0 && asset_active) {
        pxa_esp_audio_asset_sink_t asset_sink;
        if (asset_sink_snapshot(audio, &asset_sink)) {
            (void)asset_sink.control(asset_sink.context, (uint8_t)slot_index,
                                     PXA_AUDIO_ASSET_STOP, 0);
        }
    }
    if (slot_index >= 0) {
        portENTER_CRITICAL(&audio->sink_lock);
        pxa_host_audio_music_sink_t music=audio->music_sink;
        portEXIT_CRITICAL(&audio->sink_lock);
        if (music.close) music.close(music.context,provider_session);
        audio_lock(audio);
        if (audio->slots[slot_index].provider_session == provider_session &&
            audio->slots[slot_index].closing) {
            memset(&audio->slots[slot_index], 0,
                   sizeof(audio->slots[slot_index]));
        }
        audio_unlock(audio);
    }
}

static void audio_output_tick(pxa_esp_audio_state_t *audio) {
    pxa_esp_audio_sink_t sink;
    int16_t output[PXA_AUDIO_MIXER_FRAME];
#if PXA_HAS_ENCODED_AUDIO
    bool wake_encoded = false;
#endif
    if (!sink_snapshot(audio, &sink)) return;
    audio_lock(audio);
    if (!audio->suspended && audio->initialized) {
        /* The physical sink is a nonblocking bounded queue. Serialize with
         * suspension so no old frame can be submitted after its flush. */
        struct {
            uint32_t position[PXA_AUDIO_MIXER_PACKETS];
            uint32_t phase[PXA_AUDIO_MIXER_PACKETS];
            uint32_t noise[PXA_AUDIO_MIXER_PACKETS];
            uint32_t accounted[PXA_AUDIO_MIXER_PACKETS], source_phase[PXA_AUDIO_MIXER_PACKETS];
            pxa_audio_biquad_t eq[PXA_AUDIO_MAX_EQ_BANDS];
            pxa_audio_state_t state;
            uint8_t read, count, started;
            uint32_t startup_waited;
        } checkpoint[PXA_AUDIO_MIXER_VOICES];
        for (unsigned i = 0; i < PXA_AUDIO_MIXER_VOICES; ++i) {
            pxa_audio_mixer_voice_t *v = audio->voices[i];
            if (!v) continue;
            checkpoint[i].state = v->state;
            checkpoint[i].read = v->read; checkpoint[i].count = v->count;
            checkpoint[i].started = v->started; checkpoint[i].startup_waited = v->startup_waited;
            memcpy(checkpoint[i].eq, v->eq, sizeof(v->eq));
            for (unsigned j = 0; j < PXA_AUDIO_MIXER_PACKETS; ++j) {
                checkpoint[i].position[j] = v->packets[j].position;
                checkpoint[i].phase[j] = v->packets[j].phase;
                checkpoint[i].noise[j] = v->packets[j].noise;
                checkpoint[i].accounted[j] = v->packets[j].accounted_frames;
                checkpoint[i].source_phase[j] = v->packets[j].source_phase;
            }
        }
        int had_tone = 0;
        for (unsigned i = 0; i < PXA_AUDIO_MIXER_VOICES; ++i) {
            pxa_audio_mixer_voice_t *v = audio->voices[i];
            if (!v) continue;
            for (unsigned j = 0; j < PXA_AUDIO_MIXER_PACKETS; ++j)
                had_tone |= v->packets[j].position < v->packets[j].length && v->packets[j].kind;
        }
        size_t count = pxa_audio_mixer_render_voices(audio->voices, output,
                                             PXA_AUDIO_MIXER_FRAME);
#if PXA_AUDIO_LATENCY_TRACE
        /* One timestamp per starting FIFO packet, before sink submission.
         * The position check also detects a packet starting inside a frame. */
        for (unsigned i = 0; i < PXA_AUDIO_MIXER_VOICES; ++i) {
            if (!audio->voices[i]) continue;
            for (unsigned j = 0; j < PXA_AUDIO_MIXER_PACKETS; ++j) {
                const pxa_audio_mixer_packet_t *p = &audio->voices[i]->packets[j];
                if (p->kind == PXA_AUDIO_PACKET_TONE && checkpoint[i].position[j] == 0 && p->position != 0)
                    esp_log_write(ESP_LOG_INFO, "AudioTrace", "AudioTrace: render us=%lld voice=%u packet=%u duration=%u delay=%u\n",
                        (long long)esp_timer_get_time(), i, j,
                        p->meta.tone.duration_ms, p->meta.tone.delay_ms);
                if (p->kind == PXA_AUDIO_PACKET_EFFECT && checkpoint[i].position[j] == 0 && p->position != 0)
                    esp_log_write(ESP_LOG_INFO, "AudioTrace", "AudioTrace: effect_render us=%lld voice=%u packet=%u layers=%u track=%u\n",
                        (long long)esp_timer_get_time(), i, j, p->meta.effect.count, p->meta.effect.track);

            }
        }
#endif
        if (count != 0 && !sink.submit(sink.context, 0, output, PXA_AUDIO_MIXER_FRAME)) {
            ++audio->sink_rejected_frames;
            /* Backpressure preserves the exact sample and filter phase. */
            for (unsigned i = 0; i < PXA_AUDIO_MIXER_VOICES; ++i) {
                pxa_audio_mixer_voice_t *v = audio->voices[i];
                if (!v) continue;
                v->state = checkpoint[i].state;
                v->read = checkpoint[i].read; v->count = checkpoint[i].count;
                v->started = checkpoint[i].started; v->startup_waited = checkpoint[i].startup_waited;
                memcpy(v->eq, checkpoint[i].eq, sizeof(v->eq));
                for (unsigned j = 0; j < PXA_AUDIO_MIXER_PACKETS; ++j) {
                    v->packets[j].position = checkpoint[i].position[j];
                    v->packets[j].phase = checkpoint[i].phase[j];
                    v->packets[j].noise = checkpoint[i].noise[j];
                    v->packets[j].accounted_frames = checkpoint[i].accounted[j];
                    v->packets[j].source_phase = checkpoint[i].source_phase[j];
                }
            }
        } else if (count != 0) {
            ++audio->rendered_frames;
            audio->tone_frames += had_tone;
#if PXA_HAS_ENCODED_AUDIO
            wake_encoded = encoded_can_resume_locked(audio);
#endif
        }
    }
    audio_unlock(audio);
#if PXA_HAS_ENCODED_AUDIO
    if (wake_encoded) pxa_esp_encoded_audio_writable();
#endif
}

static void audio_output_task(void *argument) {
    pxa_esp_audio_state_t *audio = argument;
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        int active = 0;
        audio_lock(audio);
        if (!audio->suspended && audio->initialized) {
            for (unsigned i = 0; i < PXA_AUDIO_MIXER_VOICES; ++i) {
                const pxa_audio_mixer_voice_t *v = audio->voices[i];
                if (!v) continue;
                active |= v->count != 0;
                for (unsigned j = 0; j < v->eq_count; ++j)
                    active |= v->eq[j].z1 != 0 || v->eq[j].z2 != 0;
            }
        }
        audio_unlock(audio);
        if (!active) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            last = xTaskGetTickCount();
            continue;
        }
        audio_output_tick(audio);
        vTaskDelayUntil(&last, pdMS_TO_TICKS(PXA_ESP_AUDIO_FRAME_MS));
        if ((TickType_t)(xTaskGetTickCount() - last) > pdMS_TO_TICKS(20))
            last = xTaskGetTickCount();
    }
}

int pxa_esp_audio_set_pull_device(uint32_t rate, void (*wake)(void *), void *context) {
    if (rate < 16000 || rate > 48000 || !wake) return 0;
    audio_lock(&g_audio);
    if (g_audio.initialized || g_audio.deinitializing) { audio_unlock(&g_audio); return 0; }
    g_audio.output_rate = rate; g_audio.device_wake = wake; g_audio.device_context = context;
    audio_unlock(&g_audio);
    return 1;
}
size_t pxa_esp_audio_render(int16_t *output, size_t capacity) {
    if (!output || !capacity || capacity > PXA_AUDIO_MIXER_FRAME) return 0;
    audio_lock(&g_audio);
    size_t rendered = 0;
    if (!g_audio.suspended && g_audio.initialized) {
        int tone = 0;
        for (unsigned i = 0; i < PXA_AUDIO_MIXER_VOICES; ++i) {
            const pxa_audio_mixer_voice_t *voice = g_audio.voices[i];
            if (!voice || !voice->count || !voice->has_synthesis) continue;
            for (unsigned j = 0; j < PXA_AUDIO_MIXER_PACKETS; ++j) {
                const pxa_audio_mixer_packet_t *p = &voice->packets[j];
                tone |= p->position < p->length && p->kind != PXA_AUDIO_PACKET_PCM;
            }
        }
        rendered = pxa_audio_mixer_render_voices(g_audio.voices, output, capacity);
        if (rendered) { ++g_audio.rendered_frames; g_audio.tone_frames += tone; }
    } else memset(output, 0, capacity * sizeof(*output));
#if PXA_HAS_ENCODED_AUDIO
    const bool wake_encoded = rendered && encoded_can_resume_locked(&g_audio);
#endif
    audio_unlock(&g_audio);
#if PXA_HAS_ENCODED_AUDIO
    if (wake_encoded) pxa_esp_encoded_audio_writable();
#endif
    return rendered;
}
static bool initialize_locked(void) {
    if (g_audio.deinitializing) return false;
    if (g_audio.initialized) return true;
    if (pxa_esp_resource_memory_initialize() != PXA_STATUS_OK) return false;
    if (!g_audio.device_wake && xTaskCreateWithCaps(audio_output_task, "pxa_audio",
            PXA_ESP_AUDIO_TASK_STACK_SIZE, &g_audio, PXA_ESP_AUDIO_TASK_PRIORITY, &g_audio.task,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) return false;
    g_audio.initialized = true;
    return true;
}

int pxa_esp_audio_initialize(void) {
    audio_lock(&g_audio);
    bool ready = initialize_locked();
    audio_unlock(&g_audio);
    return ready;
}

void pxa_esp_audio_deinitialize(void) {
    audio_lock(&g_audio);
    if (g_audio.deinitializing) { audio_unlock(&g_audio); return; }
    g_audio.deinitializing = true;
    g_audio.initialized = false;
    if (g_audio.task != NULL) {
        vTaskDeleteWithCaps(g_audio.task);
        g_audio.task = NULL;
    }
    audio_unlock(&g_audio);
    pxa_esp_audio_reset_sessions();
    audio_lock(&g_audio);
    g_audio.deinitializing = false;
    audio_unlock(&g_audio);
}

#include "pxa_esp_capture.inc"

void pxa_esp_audio_reset_sessions(void) {
#if PXA_HAS_ENCODED_AUDIO
    pxa_esp_encoded_audio_detach(0);
#endif
    capture_reset_sessions();
    pxa_esp_audio_sink_t sink;
    pxa_audio_mixer_voice_t *retired[PXA_ESP_AUDIO_VOICE_COUNT];
    uint16_t active_mask = 0;
    uint16_t asset_mask = 0;
    uint16_t index;
    audio_lock(&g_audio);
    for (index = 0; index < PXA_ESP_AUDIO_VOICE_COUNT; ++index) {
        pxa_esp_audio_slot_t *slot = &g_audio.slots[index];
        if (slot->active) {
            active_mask |= (uint16_t)(UINT16_C(1) << index);
            if (slot->asset_active) asset_mask |= (uint16_t)(UINT16_C(1) << index);
            slot->active = 0;
            slot->closing = 1;
        } else if (!slot->closing) {
            slot->provider_session = 0; /* Cancel an open still preparing its storage. */
        }
        retired[index] = g_audio.voices[index];
        g_audio.voices[index] = NULL;
    }
#if PXA_HAS_ENCODED_AUDIO
    g_audio.encoded_wait_slot=0;
#endif
    g_audio.package_manifest = NULL;
    g_audio.package_root[0] = '\0';
    audio_unlock(&g_audio);
    for (index = 0; index < PXA_ESP_AUDIO_VOICE_COUNT; ++index) pxa_memory_release(retired[index]);
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
    if (sink_snapshot(&g_audio, &sink) && sink.flush) {
        for (index = 0; index < PXA_ESP_AUDIO_VOICE_COUNT; ++index)
            if (active_mask & (UINT16_C(1) << index)) sink.flush(sink.context, (uint8_t)index);
    }
    /* Board callbacks identify voices by slot. Keep slots reserved until
     * their stop/flush has returned so it cannot affect a newly opened voice. */
    audio_lock(&g_audio);
    for (index = 0; index < PXA_ESP_AUDIO_VOICE_COUNT; ++index)
        if (active_mask & (UINT16_C(1) << index)) memset(&g_audio.slots[index], 0, sizeof(g_audio.slots[index]));
    audio_unlock(&g_audio);
}

void pxa_esp_audio_set_suspended(bool suspended) {
    capture_set_suspended(suspended);
    pxa_esp_audio_sink_t pcm_sink;
    pxa_esp_audio_asset_sink_t asset_sink;
    uint16_t pcm_mask = 0;
    uint16_t asset_mask = 0;
    uint16_t index;
    audio_lock(&g_audio);
    if (g_audio.suspended == (uint8_t)suspended) {
        audio_unlock(&g_audio);
        return;
    }
    g_audio.suspended = suspended ? 1u : 0u;
    for (index = 0; index < PXA_ESP_AUDIO_VOICE_COUNT; ++index) {
        if (g_audio.slots[index].active) pcm_mask |= UINT16_C(1) << index;
        if (g_audio.slots[index].asset_active &&
            !g_audio.slots[index].asset_guest_paused)
            asset_mask |= UINT16_C(1) << index;
    }
    audio_unlock(&g_audio);
    if (asset_sink_snapshot(&g_audio, &asset_sink)) {
        for (index = 0; index < PXA_ESP_AUDIO_VOICE_COUNT; ++index) {
            if (asset_mask & (UINT16_C(1) << index))
                (void)asset_sink.control(
                    asset_sink.context, (uint8_t)index,
                    suspended ? PXA_AUDIO_ASSET_PAUSE : PXA_AUDIO_ASSET_RESUME,
                    0);
        }
    }
    if (!suspended) {
        audio_lock(&g_audio);
        audio_unlock_and_wake(&g_audio, true);
    }
    if (suspended && sink_snapshot(&g_audio, &pcm_sink) &&
        pcm_sink.flush != NULL) {
        for (index = 0; index < PXA_ESP_AUDIO_VOICE_COUNT; ++index) {
            if (pcm_mask & (UINT16_C(1) << index))
                pcm_sink.flush(pcm_sink.context, (uint8_t)index);
        }
    }
}

void pxa_esp_audio_backend(pxa_audio_backend_t *output) {
    if (output == NULL) return;
    memset(output, 0, sizeof(*output));
    output->struct_size = sizeof(*output);
    output->context = &g_audio;
    output->open_capture = capture_open;
    output->capture_io = capture_io;
    output->close_capture = capture_close;
    output->capture_frame_ms = capture_frame_ms;
    output->open = audio_open;
    output->open_pcm = audio_open_pcm;
    output->open_pcm_buffered = audio_open_pcm_buffered;
#if PXA_HAS_ENCODED_AUDIO
    output->encoded_io = audio_encoded_io;
#endif
    output->commit = audio_commit;
    output->submit = audio_submit;
    output->query = audio_query;
    output->flush = audio_flush;
    output->play_tone = audio_play_tone;
    output->play_effect = audio_play_effect;
    output->stop_effect = audio_stop_effect;
    output->play_asset = audio_play_asset;
    output->play_sound = audio_play_sound;
    output->play_sound_ex = audio_play_sound_ex;
    output->control_sound = audio_control_sound;
    output->control_music = audio_control_music;
    output->play_music = audio_play_music;
    output->playback_peek = audio_playback_peek;
    output->playback_consume = audio_playback_consume;
    output->control_asset = audio_control_asset;
    output->close = audio_close;
}

void pxa_esp_audio_snapshot(pxa_esp_audio_snapshot_t *output) {
    uint16_t index;
    if (output == NULL) return;
    memset(output, 0, sizeof(*output));
    audio_lock(&g_audio);
    if (g_audio.initialized) output->queue_capacity = PXA_AUDIO_MIXER_VOICES * PXA_AUDIO_MIXER_PACKETS;
    if (g_audio.task != NULL) output->task_stack_bytes = PXA_ESP_AUDIO_TASK_STACK_SIZE;
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
        if (g_audio.voices[index]) {
            output->queued_frames += g_audio.voices[index]->count;
            output->queue_storage_bytes += sizeof(*g_audio.voices[index]);
        }
    }
    audio_unlock(&g_audio);
}

int pxa_esp_audio_bind_package(const pxa_package_manifest_t *manifest,
                               const char *package_root) {
    size_t root_size;
    if (manifest == NULL || package_root == NULL) return 0;
    root_size = bounded_string_size(package_root,
                                    PXA_ESP_AUDIO_ASSET_PATH_MAX);
    if (root_size == 0 || root_size >= PXA_ESP_AUDIO_ASSET_PATH_MAX) return 0;
    audio_lock(&g_audio);
    g_audio.package_manifest = manifest;
    memcpy(g_audio.package_root, package_root, root_size + 1u);
    audio_unlock(&g_audio);
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

void pxa_esp_audio_set_sound_track_sink(pxa_host_audio_sound_track_fn play,
    pxa_host_audio_sound_control_fn control, void *context) {
    portENTER_CRITICAL(&g_audio.sink_lock);
    g_audio.play_sound_track = play; g_audio.control_sound = control;
    g_audio.sound_track_context = context;
    portEXIT_CRITICAL(&g_audio.sink_lock);
}
void pxa_esp_audio_set_sound_sink(pxa_host_audio_sound_fn play,void *context) {
    portENTER_CRITICAL(&g_audio.sink_lock);
    g_audio.play_sound = play; g_audio.sound_context = context;
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
