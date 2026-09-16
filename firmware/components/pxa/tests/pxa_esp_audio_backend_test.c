#include "pxa_test_platform.h"
#include <setjmp.h>
#include <string.h>
#define ESP_PLATFORM 1
#include "../src/services/pxa_esp_audio.c"

static unsigned depth, queue_count, task_count, deleted_queues, played, flushed;
static unsigned tone_frames;
static unsigned asset_plays, asset_controls, asset_stops;
static int fail_queue, fail_task;
static pxa_esp_audio_packet_t packets[PXA_ESP_AUDIO_QUEUE_LENGTH];
static jmp_buf idle;
void test_log(const char *tag, const char *format, ...) { (void)tag; (void)format; }
void test_enter(void) { assert(depth++ == 0); }
void test_leave(void) { assert(depth-- == 1); }
QueueHandle_t xQueueCreateWithCaps(unsigned count, size_t size, unsigned caps) {
    (void)caps;
    assert(depth == 0 && count == PXA_ESP_AUDIO_QUEUE_LENGTH && size == sizeof(packets[0]));
    return fail_queue ? NULL : packets;
}
void vQueueDelete(QueueHandle_t queue) { assert(queue == packets); ++deleted_queues; queue_count = 0; }
int xQueueSendToBack(QueueHandle_t queue, const void *packet, uint32_t timeout) {
    (void)timeout; assert(queue == packets && depth == 0);
    if (queue_count == PXA_ESP_AUDIO_QUEUE_LENGTH) return 0;
    packets[queue_count++] = *(const pxa_esp_audio_packet_t *)packet;
    return pdTRUE;
}
int xQueueReceive(QueueHandle_t queue, void *packet, uint32_t timeout) {
    (void)timeout; assert(queue == packets);
    if (queue_count == 0) longjmp(idle, 1);
    *(pxa_esp_audio_packet_t *)packet = packets[0];
    --queue_count;
    memmove(packets, packets + 1, queue_count * sizeof(packets[0]));
    return pdTRUE;
}
unsigned uxQueueMessagesWaiting(QueueHandle_t queue) { assert(queue == packets); return queue_count; }
unsigned uxQueueSpacesAvailable(QueueHandle_t queue) {
    assert(queue == packets);
    return PXA_ESP_AUDIO_QUEUE_LENGTH - queue_count;
}
int xTaskCreateWithCaps(void (*fn)(void *), const char *name, unsigned stack,
    void *context, unsigned priority, TaskHandle_t *task, unsigned caps) {
    (void)fn; (void)name; (void)stack; (void)priority; (void)caps;
    if (fail_task) return 0;
    ++task_count; *task = context; return pdPASS;
}
void vTaskDelete(TaskHandle_t task) { assert(task == &g_audio && task_count == 1); --task_count; }
static bool submit(void *context, uint8_t voice, const int16_t *pcm, size_t samples) {
    (void)context;
    assert(depth == 0 && voice < 3 && samples != 0);
    if (samples == 2) {
        assert(pcm[0] == 123);
        ++played;
    } else {
        assert(samples == PXA_ESP_AUDIO_FRAME_SAMPLES);
        if (tone_frames < 2) {
            for (size_t index = 0; index < samples; ++index)
                assert(pcm[index] == 0);
        } else if (tone_frames == 2) {
            assert(pcm[0] == 0 && pcm[80] != 0);
        } else if (tone_frames == 5) {
            assert(pcm[samples - 1u] == 0);
        }
        ++tone_frames;
    }
    return true;
}
static void flush(void *context, uint8_t voice) { (void)context; assert(voice < 3); ++flushed; }
static bool play_asset(void *context, uint8_t voice,
                       const char *absolute_path, bool loop,
                       int16_t gain_db_q8) {
    (void)context;
    assert(voice < 3 && strcmp(absolute_path,
                              "/pkg/audio/music.ogg") == 0 &&
           loop && gain_db_q8 == -12 * 256);
    ++asset_plays;
    return true;
}
static bool control_asset(void *context, uint8_t voice, uint8_t action,
                          int16_t gain_db_q8) {
    (void)context;
    assert(voice < 3);
    if (action == PXA_AUDIO_ASSET_STOP) {
        assert(gain_db_q8 == 0);
        ++asset_stops;
    } else {
        assert(action == PXA_AUDIO_ASSET_SET_GAIN &&
               gain_db_q8 == -18 * 256);
        ++asset_controls;
    }
    return true;
}
static void drain(void) { if (setjmp(idle) == 0) audio_output_task(&g_audio); }

int main(void) {
    pxa_audio_backend_t backend;
    pxa_audio_format_t format;
    pxa_esp_audio_snapshot_t snapshot;
    pxa_audio_graph_t graph = {0};
    pxa_audio_state_t state;
    uint64_t session;
    int16_t pcm[] = {123, -123};
    static const uint8_t asset_path[] = "audio/music.ogg";
    pxa_package_file_t package_file = {
        {asset_path, sizeof(asset_path) - 1u}, 1234, NULL};
    pxa_package_manifest_t manifest = {0};
    pxa_esp_audio_backend(&backend);
    pxa_esp_audio_snapshot(&snapshot);
    assert(snapshot.queue_storage_bytes == 0 && snapshot.task_stack_bytes == 0);
    pxa_esp_audio_set_sink(submit, flush, NULL);
    pxa_esp_audio_set_asset_sink(play_asset, control_asset, NULL);
    manifest.files = &package_file;
    manifest.file_count = 1;
    assert(pxa_esp_audio_bind_package(&manifest, "/pkg"));
    fail_queue = 1;
    assert(backend.open(backend.context, PXA_AUDIO_USAGE_MEDIA, &format, &session) == PXA_STATUS_RESOURCE_LIMIT);
    fail_queue = 0; fail_task = 1;
    assert(backend.open(backend.context, PXA_AUDIO_USAGE_MEDIA, &format, &session) == PXA_STATUS_RESOURCE_LIMIT);
    assert(g_audio.queue == NULL && deleted_queues == 1);
    fail_task = 0;
    assert(backend.open(backend.context, PXA_AUDIO_USAGE_MEDIA, &format, &session) == PXA_STATUS_OK);
    assert(task_count == 1 && format.sample_rate == 16000);
    graph.route = PXA_AUDIO_ROUTE_SPEAKER;
    assert(backend.commit(backend.context, session, &graph) == PXA_STATUS_OK);
    assert(backend.submit(backend.context, session, (const uint8_t *)pcm, sizeof(pcm)) == PXA_STATUS_OK);
    assert(backend.query(backend.context, session, &state) == PXA_STATUS_OK &&
           state.submitted_samples == 2 && state.accepted_samples == 0 &&
           state.queued_samples == 2);
    drain(); assert(played == 1);
    assert(backend.query(backend.context, session, &state) == PXA_STATUS_OK &&
           state.submitted_samples == 2 && state.accepted_samples == 2 &&
           state.queued_samples == 0 &&
           state.flags == PXA_AUDIO_STATE_ACCEPTED_IS_SINK_SUBMITTED);
    {
        pxa_audio_tone_t tone = {
            .frequency_hz = 440,
            .duration_ms = 80,
            .gain_db_q8 = -6 * 256,
            .attack_ms = 5,
            .release_ms = 30,
            .delay_ms = 40,
            .waveform = PXA_AUDIO_TONE_TRIANGLE,
        };
        assert(backend.play_tone(backend.context, session, &tone) ==
               PXA_STATUS_OK);
        drain();
        assert(tone_frames == 6);
        assert(backend.query(backend.context, session, &state) ==
                   PXA_STATUS_OK &&
               state.submitted_samples == 1922 &&
               state.accepted_samples == 1922 && state.queued_samples == 0);
    }
    {
        pxa_audio_asset_t asset = {
            asset_path, sizeof(asset_path) - 1u, -12 * 256,
            PXA_AUDIO_ASSET_LOOP};
        pxa_audio_asset_control_t control = {
            -18 * 256, PXA_AUDIO_ASSET_SET_GAIN};
        assert(backend.play_asset(backend.context, session, &asset) ==
               PXA_STATUS_OK);
        assert(asset_plays == 1);
        assert(backend.control_asset(backend.context, session, &control) ==
               PXA_STATUS_OK);
        assert(asset_controls == 1);
        control.action = PXA_AUDIO_ASSET_STOP;
        control.gain_db_q8 = 0;
        assert(backend.control_asset(backend.context, session, &control) ==
               PXA_STATUS_OK);
        assert(asset_stops == 1);
        assert(backend.control_asset(backend.context, session, &control) ==
               PXA_STATUS_BAD_STATE);
        asset.path = (const uint8_t *)"audio/missing.ogg";
        asset.path_size = strlen((const char *)asset.path);
        assert(backend.play_asset(backend.context, session, &asset) ==
               PXA_STATUS_NOT_FOUND);
    }
    queue_count = PXA_ESP_AUDIO_QUEUE_LENGTH;
    {
        pxa_audio_tone_t tone = {
            .frequency_hz = 440,
            .duration_ms = 80,
            .gain_db_q8 = -6 * 256,
            .attack_ms = 4,
            .release_ms = 4,
            .waveform = PXA_AUDIO_TONE_TRIANGLE,
        };
        assert(backend.play_tone(backend.context, session, &tone) ==
               PXA_STATUS_WOULD_BLOCK);
    }
    assert(backend.flush(backend.context, session) == PXA_STATUS_WOULD_BLOCK);
    queue_count = 0;
    assert(backend.submit(backend.context, session, (const uint8_t *)pcm, sizeof(pcm)) == PXA_STATUS_OK);
    assert(backend.flush(backend.context, session) == PXA_STATUS_OK);
    assert(backend.query(backend.context, session, &state) == PXA_STATUS_OK &&
           state.submitted_samples == 1924 && state.queued_samples == 0);
    drain();
    assert(played == 1 && flushed == 1);
    assert(backend.submit(backend.context, session, (const uint8_t *)pcm, sizeof(pcm)) == PXA_STATUS_OK);
    {
        pxa_audio_asset_t asset = {
            asset_path, sizeof(asset_path) - 1u, -12 * 256,
            PXA_AUDIO_ASSET_LOOP};
        assert(backend.play_asset(backend.context, session, &asset) ==
               PXA_STATUS_OK);
        assert(asset_plays == 2);
    }
    backend.close(backend.context, session); drain();
    assert(played == 1 && flushed == 2 && asset_stops == 2);
    pxa_esp_audio_snapshot(&snapshot);
    assert(snapshot.stale_frames == 2 && snapshot.task_stack_bytes == 4096);
    assert(snapshot.tone_commands == 1 && snapshot.tone_frames == 6 &&
           snapshot.tone_dropped_commands == 1 &&
           snapshot.voice_exhaustions == 0);
    assert(snapshot.asset_play_commands == 2 &&
           snapshot.asset_control_commands == 2 &&
           snapshot.asset_command_failures == 0);
    {
        uint64_t sessions[PXA_ESP_AUDIO_VOICE_COUNT];
        uint64_t exhausted_session;
        for (unsigned index = 0; index < PXA_ESP_AUDIO_VOICE_COUNT; ++index) {
            assert(backend.open(backend.context, PXA_AUDIO_USAGE_MEDIA,
                                &format, &sessions[index]) == PXA_STATUS_OK);
        }
        assert(backend.open(backend.context, PXA_AUDIO_USAGE_MEDIA,
                            &format, &exhausted_session) ==
               PXA_STATUS_RESOURCE_LIMIT);
        for (unsigned index = 0; index < PXA_ESP_AUDIO_VOICE_COUNT; ++index)
            backend.close(backend.context, sessions[index]);
        drain();
        pxa_esp_audio_snapshot(&snapshot);
        assert(snapshot.voice_exhaustions == 1 &&
               snapshot.active_sessions == 0);
    }
    pxa_esp_audio_deinitialize();
    pxa_esp_audio_snapshot(&snapshot);
    assert(task_count == 0 && snapshot.queue_storage_bytes == 0);
    return 0;
}
