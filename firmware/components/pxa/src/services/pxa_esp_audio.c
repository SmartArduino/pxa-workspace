#include "pxa_esp_audio.h"

#if defined(ESP_PLATFORM)

#include <limits.h>
#include <math.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "pxa/audio_mixer.h"
#include "freertos/task.h"

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
    pxa_audio_mixer_t *mixer;
    SemaphoreHandle_t mutex;
    StaticSemaphore_t mutex_storage;
    TaskHandle_t task;
    portMUX_TYPE backend_lock;
    portMUX_TYPE sink_lock;
    pxa_esp_audio_slot_t slots[PXA_ESP_AUDIO_VOICE_COUNT];
    pxa_esp_audio_sink_t sink;
    pxa_esp_audio_asset_sink_t asset_sink;
    pxa_host_audio_music_sink_t music_sink;
    pxa_host_audio_sound_fn play_sound;
    void *sound_context;
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
    audio_lock(audio);
    for (index = 0; index < PXA_ESP_AUDIO_VOICE_COUNT; ++index) {
        pxa_esp_audio_slot_t *slot = &audio->slots[index];
        if (!slot->active && !slot->closing) {
            uint64_t session = ++audio->next_provider_session;
            if (session == 0) session = ++audio->next_provider_session;
            memset(slot, 0, sizeof(*slot));
            pxa_audio_mixer_reset(&audio->mixer->voices[index]);
            slot->active = 1;
            slot->provider_session = session;
            *provider_session = session;
            format->sample_rate = PXA_ESP_AUDIO_SAMPLE_RATE;
            format->channels = 1;
            format->frame_ms = PXA_ESP_AUDIO_FRAME_MS;
            audio_unlock(audio);
            return PXA_STATUS_OK;
        }
    }
    ++audio->voice_exhaustions;
    audio_unlock(audio);
    return PXA_STATUS_RESOURCE_LIMIT;
}

static pxa_status_t audio_commit(void *context, uint64_t session,
                                  const pxa_audio_graph_t *graph) {
    pxa_esp_audio_state_t *audio = context;
    if (audio != &g_audio || !graph) return PXA_STATUS_INVALID_ARGUMENT;
    audio_lock(audio);
    int index = slot_index_locked(audio, session);
    pxa_status_t status = index < 0 ? PXA_STATUS_NOT_FOUND :
        pxa_audio_mixer_commit(&audio->mixer->voices[index], graph);
    if (status == PXA_STATUS_OK) {
        audio->slots[index].graph_committed = 1;
        audio->slots[index].linear_gain = audio->mixer->voices[index].gain;
    }
    audio_unlock(audio);
    return status;
}

static void update_queue_peak(pxa_esp_audio_state_t *audio) {
    uint32_t count = 0;
    for (unsigned i = 0; i < PXA_AUDIO_MIXER_VOICES; ++i)
        count += audio->mixer->voices[i].count;
    if (count > audio->peak_queued_frames) audio->peak_queued_frames = count;
}

static pxa_status_t audio_submit(void *context, uint64_t session,
                                  const uint8_t *pcm, size_t size) {
    pxa_esp_audio_state_t *audio = context;
    if (audio != &g_audio) return PXA_STATUS_INVALID_ARGUMENT;
    audio_lock(audio);
    int index = slot_index_locked(audio, session);
    pxa_status_t status = index < 0 ? PXA_STATUS_NOT_FOUND :
        pxa_audio_mixer_write(&audio->mixer->voices[index], pcm, size);
    if (status == PXA_STATUS_OK) { ++audio->submitted_frames; update_queue_peak(audio); }
    if (status == PXA_STATUS_WOULD_BLOCK) ++audio->queue_full_frames;
    audio_unlock(audio);
    if (status == PXA_STATUS_OK) xTaskNotifyGive(audio->task);
    return status;
}

static pxa_status_t audio_play_tone(void *context, uint64_t session,
                                     const pxa_audio_tone_t *tone) {
    pxa_esp_audio_state_t *audio = context;
    if (audio != &g_audio) return PXA_STATUS_INVALID_ARGUMENT;
    audio_lock(audio);
    int index = slot_index_locked(audio, session);
    pxa_status_t status = index < 0 ? PXA_STATUS_NOT_FOUND :
        pxa_audio_mixer_tone(&audio->mixer->voices[index], tone);
    if (status == PXA_STATUS_OK) {
        ++audio->submitted_frames;
        ++audio->tone_commands;
        update_queue_peak(audio);
    }
    if (status == PXA_STATUS_WOULD_BLOCK) {
        ++audio->queue_full_frames;
        ++audio->tone_dropped_commands;
    }
    audio_unlock(audio);
    if (status == PXA_STATUS_OK) xTaskNotifyGive(audio->task);
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
        *state = audio->mixer->voices[slot_index].state;
    }
    audio_unlock(audio);
    return slot_index >= 0 ? PXA_STATUS_OK : PXA_STATUS_NOT_FOUND;
}

static pxa_status_t audio_flush(void *context, uint64_t session) {
    pxa_esp_audio_state_t *audio = context;
    if (audio != &g_audio) return PXA_STATUS_INVALID_ARGUMENT;
    audio_lock(audio);
    int index = slot_index_locked(audio, session);
    if (index >= 0) {
        pxa_audio_mixer_flush(&audio->mixer->voices[index]);
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
    uint8_t asset_active = 0;
    if (audio != &g_audio) return;
    audio_lock(audio);
    slot_index = slot_index_locked(audio, provider_session);
    if (slot_index >= 0) {
        asset_active = audio->slots[slot_index].asset_active;
        pxa_audio_mixer_reset(&audio->mixer->voices[slot_index]);
        audio->slots[slot_index].active = 0;
        audio->slots[slot_index].closing = 1;
    }
    audio_unlock(audio);
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
    if (!sink_snapshot(audio, &sink)) return;
    audio_lock(audio);
    if (!audio->suspended && audio->mixer != NULL) {
        /* The physical sink is a nonblocking bounded queue. Serialize with
         * suspension so no old frame can be submitted after its flush. */
        struct {
            uint32_t position[PXA_AUDIO_MIXER_PACKETS];
            uint32_t phase[PXA_AUDIO_MIXER_PACKETS];
            uint32_t noise[PXA_AUDIO_MIXER_PACKETS];
            pxa_audio_biquad_t eq[PXA_AUDIO_MAX_EQ_BANDS];
            pxa_audio_state_t state;
            uint8_t read, count;
        } checkpoint[PXA_AUDIO_MIXER_VOICES];
        for (unsigned i = 0; i < PXA_AUDIO_MIXER_VOICES; ++i) {
            pxa_audio_mixer_voice_t *v = &audio->mixer->voices[i];
            checkpoint[i].state = v->state;
            checkpoint[i].read = v->read; checkpoint[i].count = v->count;
            memcpy(checkpoint[i].eq, v->eq, sizeof(v->eq));
            for (unsigned j = 0; j < PXA_AUDIO_MIXER_PACKETS; ++j) {
                checkpoint[i].position[j] = v->packets[j].position;
                checkpoint[i].phase[j] = v->packets[j].phase;
                checkpoint[i].noise[j] = v->packets[j].noise;
            }
        }
        int had_tone = 0;
        for (unsigned i = 0; i < PXA_AUDIO_MIXER_VOICES; ++i) {
            pxa_audio_mixer_voice_t *v = &audio->mixer->voices[i];
            had_tone |= v->count && v->packets[v->read].is_tone;
        }
        size_t count = pxa_audio_mixer_render(audio->mixer, output,
                                             PXA_AUDIO_MIXER_FRAME);
        if (count != 0 && !sink.submit(sink.context, 0, output, PXA_AUDIO_MIXER_FRAME)) {
            ++audio->sink_rejected_frames;
            /* Backpressure preserves the exact sample and filter phase. */
            for (unsigned i = 0; i < PXA_AUDIO_MIXER_VOICES; ++i) {
                pxa_audio_mixer_voice_t *v = &audio->mixer->voices[i];
                v->state = checkpoint[i].state;
                v->read = checkpoint[i].read; v->count = checkpoint[i].count;
                memcpy(v->eq, checkpoint[i].eq, sizeof(v->eq));
                for (unsigned j = 0; j < PXA_AUDIO_MIXER_PACKETS; ++j) {
                    v->packets[j].position = checkpoint[i].position[j];
                    v->packets[j].phase = checkpoint[i].phase[j];
                    v->packets[j].noise = checkpoint[i].noise[j];
                }
            }
        } else if (count != 0) {
            ++audio->rendered_frames;
            audio->tone_frames += had_tone;
        }
    }
    audio_unlock(audio);
}

static void audio_output_task(void *argument) {
    pxa_esp_audio_state_t *audio = argument;
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        int active = 0;
        audio_lock(audio);
        if (!audio->suspended && audio->mixer) {
            for (unsigned i = 0; i < PXA_AUDIO_MIXER_VOICES; ++i) {
                const pxa_audio_mixer_voice_t *v = &audio->mixer->voices[i];
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

int pxa_esp_audio_initialize(void) {
    if (g_audio.mixer != NULL && g_audio.task != NULL) return 1;
    g_audio.mixer = heap_caps_calloc(1, sizeof(*g_audio.mixer),
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (g_audio.mixer == NULL) return 0;
    if (xTaskCreateWithCaps(audio_output_task, "pxa_audio",
            PXA_ESP_AUDIO_TASK_STACK_SIZE, &g_audio,
            PXA_ESP_AUDIO_TASK_PRIORITY, &g_audio.task,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
        heap_caps_free(g_audio.mixer);
        g_audio.mixer = NULL;
        return 0;
    }
    return 1;
}

void pxa_esp_audio_deinitialize(void) {
    audio_lock(&g_audio);
    if (g_audio.task != NULL) {
        vTaskDelete(g_audio.task);
        g_audio.task = NULL;
    }
    audio_unlock(&g_audio);
    pxa_esp_audio_reset_sessions();
    heap_caps_free(g_audio.mixer);
    g_audio.mixer = NULL;
}

void pxa_esp_audio_reset_sessions(void) {
    pxa_esp_audio_sink_t sink;
    uint16_t active_mask = 0;
    uint16_t asset_mask = 0;
    uint16_t index;
    audio_lock(&g_audio);
    for (index = 0; index < PXA_ESP_AUDIO_VOICE_COUNT; ++index) {
        if (g_audio.slots[index].active || g_audio.slots[index].closing) {
            active_mask |= (uint16_t)(UINT16_C(1) << index);
        }
        if (g_audio.slots[index].asset_active)
            asset_mask |= (uint16_t)(UINT16_C(1) << index);
    }
    memset(g_audio.slots, 0, sizeof(g_audio.slots));
    if (g_audio.mixer) memset(g_audio.mixer, 0, sizeof(*g_audio.mixer));
    g_audio.package_manifest = NULL;
    g_audio.package_root[0] = '\0';
    audio_unlock(&g_audio);
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

void pxa_esp_audio_set_suspended(bool suspended) {
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
    if (!suspended && g_audio.task) xTaskNotifyGive(g_audio.task);
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
    output->open = audio_open;
    output->commit = audio_commit;
    output->submit = audio_submit;
    output->query = audio_query;
    output->flush = audio_flush;
    output->play_tone = audio_play_tone;
    output->play_asset = audio_play_asset;
    output->play_sound = audio_play_sound;
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
    if (g_audio.mixer != NULL) {
        output->queue_capacity = PXA_AUDIO_MIXER_VOICES * PXA_AUDIO_MIXER_PACKETS;
        output->queue_storage_bytes = sizeof(*g_audio.mixer);
    }
    if (g_audio.task != NULL) output->task_stack_bytes = PXA_ESP_AUDIO_TASK_STACK_SIZE;
    audio_lock(&g_audio);
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
        if (g_audio.mixer) output->queued_frames += g_audio.mixer->voices[index].count;
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
