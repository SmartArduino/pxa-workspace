#include "pxa_test_platform.h"
#include <string.h>
#define ESP_PLATFORM 1
#include "../src/services/pxa_esp_audio.c"

static unsigned depth, locked, played, stopped, paused, resumed;
static unsigned sound_plays;
static uint64_t music_session;
static unsigned music_plays, music_closes;
static bool music_initially_paused;
static pxa_audio_playback_event_t music_event;
static int reject, fail_alloc, fail_task;
static int16_t rendered[PXA_AUDIO_MIXER_FRAME];
void test_log(const char *tag, const char *format, ...) { (void)tag; (void)format; }
void test_enter(void) { assert(depth++ == 0); }
void test_leave(void) { assert(depth-- == 1); }
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *s) { return s; }
int xSemaphoreTake(SemaphoreHandle_t s, uint32_t t) {
    (void)s; (void)t; assert(!depth && !locked); locked = 1; return 1;
}
int xSemaphoreGive(SemaphoreHandle_t s) { (void)s; assert(locked); locked = 0; return 1; }
void *heap_caps_calloc(size_t n, size_t size, unsigned caps) {
    (void)caps; return fail_alloc ? NULL : calloc(n, size);
}
void heap_caps_free(void *p) { free(p); }
int xTaskCreateWithCaps(void (*fn)(void *), const char *name, unsigned stack,
    void *context, unsigned priority, TaskHandle_t *task, unsigned caps) {
    (void)fn; (void)name; (void)stack; (void)priority; (void)caps;
    if (fail_task) return 0;
    *task = context; return pdPASS;
}
void vTaskDelete(TaskHandle_t task) { assert(task == &g_audio); }
void xTaskNotifyGive(TaskHandle_t task) { (void)task; }
uint32_t ulTaskNotifyTake(int clear, TickType_t wait) { (void)clear; (void)wait; return 0; }
TickType_t xTaskGetTickCount(void) { return 0; }
void vTaskDelayUntil(TickType_t *a, TickType_t b) { (void)a; (void)b; }
static bool submit(void *context, uint8_t voice, const int16_t *pcm, size_t n) {
    (void)context; assert(!depth && voice == 0 && n == 320);
    if (reject) return false;
    memcpy(rendered, pcm, n * 2); ++played; return true;
}
static void flush(void *context, uint8_t voice) { (void)context; (void)voice; }
static bool asset_play(void *context, uint8_t voice, const char *path, bool loop, int16_t gain) {
    (void)context; (void)voice; (void)loop; (void)gain;
    assert(strcmp(path, "/pkg/assets/test.ogg") == 0); return true;
}
static bool asset_control(void *context, uint8_t voice, uint8_t action, int16_t gain) {
    (void)context; (void)voice; (void)gain;
    stopped += action == PXA_AUDIO_ASSET_STOP;
    paused += action == PXA_AUDIO_ASSET_PAUSE;
    resumed += action == PXA_AUDIO_ASSET_RESUME;
    return true;
}
static void *sound_allocate(void *c, size_t n) { (void)c; assert(!locked && !depth); return malloc(n); }
static void sound_free(void *c, void *p) { (void)c; assert(!locked && !depth); free(p); }
static bool sound_play(void *c, uint8_t voice, pxa_asset_object_t *sound, int16_t gain) {
    (void)c; assert(!locked && !depth && voice == 1 && gain == -6*256);
    pxa_asset_object_view_t view;
    pxa_asset_object_view(sound, &view);
    assert(view.kind == PXA_ASSET_AUDIO && view.bytes == 160 && view.data[0] == 173);
    if (reject) return false;
    ++sound_plays;
    return true;
}
static pxa_status_t music_play(void *ctx, uint8_t voice, uint64_t session,
    const char *path, bool loop, int16_t gain, bool initial_pause, uint64_t *instance) {
    assert(ctx == &music_event && !locked && !depth && voice == 1);
    assert(session == music_session && loop && gain == -6*256);
    assert(!strcmp(path,"/pkg/assets/test.ogg"));
    *instance=0;
    if (reject) return PXA_STATUS_WOULD_BLOCK;
    music_initially_paused=initial_pause; ++music_plays;
    *instance=UINT64_C(0x1234567800000001);
    music_event=(pxa_audio_playback_event_t){.instance=*instance,
        .provider_session=session,.state=PXA_AUDIO_PLAYBACK_READY,.status=PXA_STATUS_OK};
    return PXA_STATUS_OK;
}
static pxa_status_t music_peek(void *ctx, pxa_audio_playback_event_t *event) {
    assert(ctx == &music_event && !locked && !depth);
    if (!music_event.instance) return PXA_STATUS_NOT_FOUND;
    *event=music_event; return PXA_STATUS_OK;
}
static pxa_status_t music_consume(void *ctx, const pxa_audio_playback_event_t *event) {
    assert(ctx == &music_event && !locked && !depth);
    assert(event->instance==music_event.instance && event->provider_session==music_session);
    music_event.instance=0; return PXA_STATUS_OK;
}
static void music_close(void *ctx, uint64_t session) {
    assert(ctx == &music_event && !locked && !depth);
    if (session==music_session) { ++music_closes; music_event.instance=0; }
}
int main(void) {
    pxa_audio_backend_t b;
    pxa_audio_format_t f;
    pxa_audio_graph_t g = {.route = PXA_AUDIO_ROUTE_SPEAKER};
    pxa_audio_state_t state;
    uint64_t a, c, d, exhausted;
    uint8_t pcm[640];
    for (unsigned i = 0; i < 320; ++i) { pcm[2*i] = 0xe8; pcm[2*i+1] = 3; }
    pxa_esp_audio_backend(&b);
    pxa_esp_audio_set_sink(submit, flush, NULL);
    fail_alloc = 1;
    assert(b.open(b.context, 1, &f, &a) == PXA_STATUS_RESOURCE_LIMIT);
    fail_alloc = 0; fail_task = 1;
    assert(b.open(b.context, 1, &f, &a) == PXA_STATUS_RESOURCE_LIMIT);
    assert(!g_audio.mixer); fail_task = 0;
    assert(b.open(b.context, 1, &f, &a) == PXA_STATUS_OK);
    assert(b.open(b.context, 1, &f, &c) == PXA_STATUS_OK && a != c);
    assert(b.open(b.context, 1, &f, &d) == PXA_STATUS_OK);
    assert(b.open(b.context, 1, &f, &exhausted) == PXA_STATUS_RESOURCE_LIMIT);
    assert(b.submit(b.context, a, pcm, sizeof(pcm)) == PXA_STATUS_BAD_STATE);
    assert(b.commit(b.context, a, &g) == PXA_STATUS_OK);
    assert(b.commit(b.context, c, &g) == PXA_STATUS_OK);
    assert(b.submit(b.context, a, pcm, sizeof(pcm)) == PXA_STATUS_OK);
    assert(b.submit(b.context, c, pcm, sizeof(pcm)) == PXA_STATUS_OK);
    reject = 1; audio_output_tick(&g_audio);
    assert(b.query(b.context, a, &state) == PXA_STATUS_OK);
    assert(state.accepted_samples == 0 && state.queued_samples == 320);
    reject = 0; audio_output_tick(&g_audio);
    assert(played == 1 && rendered[100] == 2000);
    assert(b.query(b.context, a, &state) == PXA_STATUS_OK);
    assert(state.accepted_samples == 320 && state.queued_samples == 0);
    for (unsigned i = 0; i < 4; ++i)
        assert(b.submit(b.context, a, pcm, sizeof(pcm)) == PXA_STATUS_OK);
    assert(b.submit(b.context, a, pcm, sizeof(pcm)) == PXA_STATUS_WOULD_BLOCK);
    assert(b.flush(b.context, a) == PXA_STATUS_OK); /* must work while full */
    assert(b.submit(b.context, c, pcm, sizeof(pcm)) == PXA_STATUS_OK);
    b.close(b.context, a); audio_output_tick(&g_audio);
    assert(rendered[100] == 1000); /* closing another voice cannot erase c */
    pxa_audio_tone_t tone = {.frequency_hz=440, .duration_ms=1000,
        .gain_db_q8=-6*256, .attack_ms=5, .release_ms=10};
    assert(b.play_tone(b.context, c, &tone) == PXA_STATUS_OK);
    audio_output_tick(&g_audio);
    assert(b.query(b.context, c, &state) == PXA_STATUS_OK && state.queued_samples == 15680);
    pxa_esp_audio_set_suspended(true);
    for (unsigned i = 0; i < 5; ++i) audio_output_tick(&g_audio);
    assert(b.query(b.context, c, &state) == PXA_STATUS_OK && state.queued_samples == 15680);
    pxa_esp_audio_set_suspended(false);
    for (unsigned i = 0; i < 49; ++i) audio_output_tick(&g_audio);
    assert(b.query(b.context, c, &state) == PXA_STATUS_OK && state.queued_samples == 0);
    assert(state.accepted_samples == 16640);
    pxa_esp_audio_set_asset_sink(asset_play, asset_control, NULL);
    static const uint8_t path[] = "assets/test.ogg";
    pxa_package_file_t file = {{path, sizeof(path)-1}, 1234, NULL};
    pxa_package_manifest_t manifest = {.files=&file, .file_count=1};
    assert(pxa_esp_audio_bind_package(&manifest, "/pkg"));
    pxa_audio_asset_t asset = {path, sizeof(path)-1, -6*256, 1};
    assert(b.play_asset(b.context, c, &asset) == PXA_STATUS_OK);
    pxa_esp_audio_set_suspended(true); assert(paused == 1);
    pxa_esp_audio_set_suspended(false); assert(resumed == 1);
    pxa_audio_asset_control_t control = {0, PXA_AUDIO_ASSET_PAUSE};
    assert(b.control_asset(b.context, c, &control) == PXA_STATUS_OK);
    pxa_esp_audio_set_suspended(true); pxa_esp_audio_set_suspended(false);
    assert(resumed == 1); /* guest pause survives focus round trip */
    pxa_asset_info_t sound_info = {0};
    sound_info.kind = PXA_ASSET_AUDIO;
    sound_info.encoding = PXA_ASSET_ENCODING_PCM_U8_16K_MONO;
    sound_info.stored_bytes = sound_info.decoded_bytes = 160;
    pxa_asset_object_t *sound; uint8_t *sound_data;
    assert(!pxa_asset_object_create(&sound_info, sound_allocate, sound_free, NULL, &sound, &sound_data));
    memset(sound_data, 173, 160);
    assert(b.play_sound(b.context,c,sound,-6*256) == PXA_STATUS_UNSUPPORTED);
    pxa_esp_audio_set_sound_sink(sound_play, NULL);
    assert(b.play_sound(b.context,d,sound,-6*256) == PXA_STATUS_BAD_STATE);
    assert(b.play_sound(b.context,a,sound,-6*256) == PXA_STATUS_NOT_FOUND);
    reject = 1;
    assert(b.play_sound(b.context,c,sound,-6*256) == PXA_STATUS_WOULD_BLOCK);
    assert(!sound_plays); reject = 0;
    assert(!b.play_sound(b.context,c,sound,-6*256) && sound_plays == 1);
    unsigned before_pause = paused, before_resume = resumed;
    pxa_esp_audio_set_suspended(true);
    assert(paused == before_pause + 1);
    assert(!b.play_sound(b.context,c,sound,-6*256) && sound_plays == 2);
    assert(paused == before_pause + 2);
    pxa_esp_audio_set_suspended(false);
    assert(resumed == before_resume + 1);
    pxa_asset_object_release(sound);
    uint64_t instance=99;
    assert(b.play_music(b.context,c,&asset,&instance)==PXA_STATUS_UNSUPPORTED && !instance);
    music_session=c;
    pxa_host_audio_music_sink_t music={.context=&music_event,.play=music_play,
        .peek=music_peek,.consume=music_consume,.close=music_close};
    pxa_esp_audio_set_music_sink(&music);
    assert(b.play_music(b.context,a,&asset,&instance)==PXA_STATUS_NOT_FOUND && !instance);
    assert(b.play_music(b.context,d,&asset,&instance)==PXA_STATUS_BAD_STATE && !instance);
    pxa_esp_audio_set_suspended(true);
    assert(!b.play_music(b.context,c,&asset,&instance));
    assert(instance==UINT64_C(0x1234567800000001) && music_plays==1 && music_initially_paused);
    pxa_audio_playback_event_t event;
    assert(!b.playback_peek(b.context,&event) && event.instance==instance && event.provider_session==c);
    reject=1;
    assert(b.play_music(b.context,c,&asset,&instance)==PXA_STATUS_WOULD_BLOCK && !instance);
    assert(music_plays==1 && g_audio.slots[1].asset_active);
    reject=0;
    assert(!b.playback_consume(b.context,&event));
    assert(b.playback_peek(b.context,&event)==PXA_STATUS_NOT_FOUND);
    pxa_esp_audio_set_suspended(false);
    assert(!b.play_music(b.context,c,&asset,&instance) && !music_initially_paused);
    static const uint8_t old_pcm[] = "assets/test.pcm";
    asset.path = old_pcm; asset.path_size = sizeof(old_pcm)-1;
    assert(b.play_asset(b.context,c,&asset) == PXA_STATUS_UNSUPPORTED);
    b.close(b.context, c); b.close(b.context, d); assert(stopped >= 1);
    assert(music_closes==1 && b.playback_peek(b.context,&event)==PXA_STATUS_NOT_FOUND);
    pxa_esp_audio_deinitialize();
    assert(!g_audio.mixer && !g_audio.task && !locked);
    return 0;
}
