#define _GNU_SOURCE
#undef NDEBUG
#include "pxa_test_platform.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <mbedtls/md.h>

#define ESP_PLATFORM 1
#include "../src/services/pxa_esp_resource_memory.c"
#include "../src/services/pxa_esp_assets.c"

static pthread_mutex_t budget_mutex = PTHREAD_MUTEX_INITIALIZER;
static __thread unsigned budget_lock_depth;
void test_enter(void) { assert(!budget_lock_depth); pthread_mutex_lock(&budget_mutex); ++budget_lock_depth; }
void test_leave(void) { assert(budget_lock_depth == 1); --budget_lock_depth; pthread_mutex_unlock(&budget_mutex); }

static pthread_mutex_t cache_mutex = PTHREAD_MUTEX_INITIALIZER;
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    unsigned signals;
    int exit;
} notification_t;
static notification_t worker_notification={PTHREAD_MUTEX_INITIALIZER,PTHREAD_COND_INITIALIZER,0,0};
static __thread notification_t local_notification;
static __thread notification_t *current_notification;
#define task_mutex worker_notification.mutex
#define task_condition worker_notification.condition
#define task_exit worker_notification.exit
static pthread_mutex_t io_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t io_condition = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t memory_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t task_thread;
static unsigned task_count;
static int io_gate, io_entered, io_trace_enabled;
static unsigned io_trace[64], io_trace_count;
static pxa_status_t io_fail_once;
static __thread unsigned cache_lock_depth;
static struct {void *pointer; size_t bytes; unsigned caps;} allocations[96];
static size_t live_allocations, live_bytes, peak_pixel_bytes[2], pixel_bytes[2];
static unsigned notices;
static int64_t fake_clock_us;
static unsigned fake_delays;

int64_t esp_timer_get_time(void) {
    struct timespec now;
    if (fake_clock_us) return fake_clock_us;
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return (int64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
}

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *s) { (void)s; return &cache_mutex; }
int xSemaphoreTake(SemaphoreHandle_t s, uint32_t timeout) {
    (void)timeout; assert(s == &cache_mutex && !cache_lock_depth);
    pthread_mutex_lock(s); ++cache_lock_depth; return 1;
}
int xSemaphoreGive(SemaphoreHandle_t s) {
    assert(s == &cache_mutex && cache_lock_depth == 1); --cache_lock_depth;
    pthread_mutex_unlock(s); return 1;
}
TaskHandle_t xTaskGetCurrentTaskHandle(void) {
    if (!current_notification) {
        assert(!pthread_mutex_init(&local_notification.mutex,NULL));
        assert(!pthread_cond_init(&local_notification.condition,NULL));
        current_notification=&local_notification;
    }
    return current_notification;
}
static struct timespec after_ms(uint32_t ms) {
    struct timespec t; assert(!clock_gettime(CLOCK_REALTIME,&t));
    t.tv_sec+=ms/1000; t.tv_nsec+=(long)(ms%1000)*1000000;
    if(t.tv_nsec>=1000000000) { ++t.tv_sec; t.tv_nsec-=1000000000; }
    return t;
}
static void *task_entry(void *context) {
    current_notification=&worker_notification; run_worker(context); return NULL;
}
int xTaskCreateWithCaps(void (*fn)(void *), const char *name, unsigned stack, void *ctx,
    unsigned priority, TaskHandle_t *out, unsigned caps) {
    assert(fn == run_worker && !strcmp(name, "pxa_assets") && stack == 6144 && priority == 3);
    assert(caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    ++task_count; *out = &worker_notification;
    return pthread_create(&task_thread, NULL, task_entry, ctx) == 0 ? pdPASS : 0;
}
void xTaskNotifyGive(TaskHandle_t handle) {
    notification_t *n=handle; assert(n);
    pthread_mutex_lock(&n->mutex); ++n->signals;
    pthread_cond_signal(&n->condition); pthread_mutex_unlock(&n->mutex);
}
uint32_t ulTaskNotifyTake(int clear, TickType_t timeout) {
    assert(!cache_lock_depth);
    notification_t *n=xTaskGetCurrentTaskHandle();
    struct timespec deadline=after_ms(timeout==portMAX_DELAY ? 0 : timeout);
    pthread_mutex_lock(&n->mutex);
    while (!n->signals && !n->exit) {
        int result=timeout==portMAX_DELAY ? pthread_cond_wait(&n->condition,&n->mutex) :
            pthread_cond_timedwait(&n->condition,&n->mutex,&deadline);
        if (result==ETIMEDOUT) { pthread_mutex_unlock(&n->mutex); return 0; }
        assert(!result);
    }
    if (n->exit) { pthread_mutex_unlock(&n->mutex); pthread_exit(NULL); }
    unsigned count=n->signals; n->signals=clear ? 0 : n->signals-1;
    pthread_mutex_unlock(&n->mutex); return count;
}
void vTaskDelay(TickType_t ticks) {
    if (fake_clock_us) { assert(ticks == 1); ++fake_delays; fake_clock_us += 10000; return; }
    struct timespec time = {0, (long)ticks * 1000000}; nanosleep(&time, NULL);
}
void *heap_caps_malloc(size_t bytes, unsigned caps) {
    assert(!cache_lock_depth && !budget_lock_depth);
    void *memory = malloc(bytes); assert(memory);
    pthread_mutex_lock(&memory_mutex);
    unsigned i; for (i = 0; i < 96 && allocations[i].pointer; ++i) {}
    assert(i < 96); allocations[i].pointer = memory; allocations[i].bytes = bytes; allocations[i].caps = caps;
    ++live_allocations; live_bytes += bytes;
    if (bytes == pxa_memory_allocation_bytes(65568) || bytes == pxa_memory_allocation_bytes(544)) {
        assert(pthread_equal(pthread_self(), task_thread));
        unsigned cls = bytes == pxa_memory_allocation_bytes(65568) ? 1 : 0;
        assert(caps == ((cls ? MALLOC_CAP_SPIRAM : MALLOC_CAP_INTERNAL) | MALLOC_CAP_8BIT));
        pixel_bytes[cls] += bytes;
        if (pixel_bytes[cls] > peak_pixel_bytes[cls]) peak_pixel_bytes[cls] = pixel_bytes[cls];
        assert(pixel_bytes[1] <= 512 * 1024 && pixel_bytes[0] <= 64 * 1024);
    }
    pthread_mutex_unlock(&memory_mutex); return memory;
}
void heap_caps_free(void *memory) {
    assert(!cache_lock_depth && !budget_lock_depth); if (!memory) return;
    pthread_mutex_lock(&memory_mutex);
    unsigned i; for (i = 0; i < 96 && allocations[i].pointer != memory; ++i) {}
    assert(i < 96);
    size_t bytes = allocations[i].bytes;
    if (bytes == pxa_memory_allocation_bytes(65568) || bytes == pxa_memory_allocation_bytes(544)) {
        assert(pthread_equal(pthread_self(), task_thread));
        pixel_bytes[bytes == pxa_memory_allocation_bytes(65568) ? 1 : 0] -= bytes;
    }
    live_bytes -= bytes; --live_allocations; allocations[i].pointer = NULL;
    pthread_mutex_unlock(&memory_mutex); free(memory);
}
pxa_status_t pxa_esp_mbedtls_sha256(const uint8_t *data, size_t bytes, uint8_t *out) {
    return mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), data, bytes, out) ? PXA_STATUS_INTERNAL : PXA_STATUS_OK;
}
static pxa_status_t before_read(void *context,unsigned lane,size_t capacity,
    void *cancel_context,int (*cancelled)(void *)) {
    (void)context; assert(!cache_lock_depth && lane<=1 && capacity<=4096);
    pxa_status_t status=PXA_STATUS_OK;
    pthread_mutex_lock(&io_mutex);
    if (io_trace_enabled && io_trace_count<64) io_trace[io_trace_count++]=lane;
    if (io_gate) {
        io_entered=1; pthread_cond_signal(&io_condition);
        while (io_gate) {
            pthread_mutex_unlock(&io_mutex);
            int stop=cancelled(cancel_context);
            pthread_mutex_lock(&io_mutex);
            if (stop) { status=PXA_STATUS_CANCELLED; break; }
            struct timespec deadline=after_ms(10);
            int result=pthread_cond_timedwait(&io_condition,&io_mutex,&deadline);
            assert(!result || result==ETIMEDOUT);
        }
    }
    if (!status && io_fail_once) { status=io_fail_once; io_fail_once=0; }
    pthread_mutex_unlock(&io_mutex);
    return status;
}
static void block_io(void) { pthread_mutex_lock(&io_mutex); io_gate = 1; io_entered = 0; pthread_mutex_unlock(&io_mutex); }
static void wait_io(void) {
    pthread_mutex_lock(&io_mutex);
    while (!io_entered) pthread_cond_wait(&io_condition, &io_mutex);
    pthread_mutex_unlock(&io_mutex);
}
static void resume_io(void) { pthread_mutex_lock(&io_mutex); io_gate = 0; pthread_cond_signal(&io_condition); pthread_mutex_unlock(&io_mutex); }
static void notify(void *ctx) { (void)ctx; __atomic_add_fetch(&notices, 1, __ATOMIC_RELAXED); }
static pxa_bytes_t bytes(const char *text) { return (pxa_bytes_t){(const uint8_t *)text, strlen(text)}; }
#include "../../../../deps/pxa-system/libpxa/tests/asset_read_worker_checks.h"
static void blob_delay(void) { vTaskDelay(1); }
static pxa_raster_asset_t *await_asset(pxa_assets_backend_t *backend, pxa_asset_ticket_t ticket) {
    for (unsigned i = 0; i < 15000; ++i) {
        pxa_raster_asset_t *asset = NULL;
        pxa_status_t status = backend->acquire(NULL, 1, ticket, &asset);
        if (!status) return asset;
        assert(status == PXA_STATUS_WOULD_BLOCK); vTaskDelay(1);
    }
    assert(!"asset timeout"); return NULL;
}
static void end_activation(void) {
    for (unsigned i = 0; i < 15000; ++i) {
        pxa_status_t status = pxa_esp_assets_end();
        if (!status) { assert(!live_allocations && !live_bytes); assert(!pxa_esp_resource_memory_end()); return; }
        assert(status == PXA_STATUS_WOULD_BLOCK); vTaskDelay(1);
    }
    assert(!"shutdown timeout");
}
static void draw_scene(pxa_raster_bindings_t *bindings, unsigned first) {
    pxa_raster_resources_t view; pxa_raster_bindings_view(bindings, PXA_RASTER_CAP_KNOWN_MASK, &view);
    uint8_t draw[PXA_RASTER_DRAW_HEADER_BYTES + PXA_RASTER_SPRITE_BYTES * 4] = {0};
    uint16_t pixels[16]; pxa_raster_target_t target = {0}; pxa_raster_draw_list_view_t list;
    pxa_write_u32(draw, PXA_RASTER_DRAW_MAGIC); pxa_write_u16(draw + 4, PXA_RASTER_ABI_MAJOR);
    pxa_write_u16(draw + 6, PXA_RASTER_ABI_MINOR); pxa_write_u32(draw + 8, sizeof(draw));
    pxa_write_u32(draw + 16, 4); pxa_write_u64(draw + 20, 1);
    for (unsigned i = 0; i < 4; ++i) {
        uint8_t *r = draw + PXA_RASTER_DRAW_HEADER_BYTES + i * PXA_RASTER_SPRITE_BYTES;
        r[0] = PXA_RASTER_RECORD_SPRITE; r[4] = (uint8_t)i; pxa_write_u16(r + 2, PXA_RASTER_SPRITE_BYTES);
        pxa_write_u16(r + 8, (uint16_t)((i % 2) * 2)); pxa_write_u16(r + 10, (uint16_t)((i / 2) * 2));
        pxa_write_u16(r + 12, 2); pxa_write_u16(r + 14, 2); pxa_write_u16(r + 20, 256); pxa_write_u16(r + 22, 256);
    }
    target.pixels = pixels; target.width = target.height = target.stride_pixels = 4;
    target.scratch_mode = PXA_RASTER_SCRATCH_NONE;
    assert(pxa_raster_validate_draw_list(draw, sizeof(draw), &target, &view, &list) == 0);
    pxa_raster_execute_draw_list(draw, &list, &target, &view, NULL);
    for (unsigned y = 0; y < 4; ++y) for (unsigned x = 0; x < 4; ++x)
        assert(pixels[y * 4 + x] == (first + y / 2 * 2 + x / 2 + 1) * 251);
}

typedef struct { pxa_esp_music_input_t *input; pxa_status_t status; size_t count; uint8_t data[64]; int done; } music_read_t;
static void *blocked_music_read(void *context) {
    music_read_t *read = context;
    memset(read->data, 0x5a, sizeof(read->data));
    read->status = pxa_esp_music_input_read(read->input, read->data, sizeof(read->data), &read->count);
    __atomic_store_n(&read->done,1,__ATOMIC_RELEASE);
    return NULL;
}
static int cancel_music(void *context) { return __atomic_load_n((int *)context,__ATOMIC_RELAXED); }
static void check_music_inputs(const pxa_esp_assets_config_t *config) {
    pxa_assets_backend_t backend;
    const pxa_memory_allocator_t *allocator;
    pxa_esp_music_input_t *input = NULL, *rejected = NULL;
    char path[1024], moved[1030];
    snprintf(path, sizeof(path), "%s/assets/zzmusic.ogg", config->package_root);
    snprintf(moved, sizeof(moved), "%s.moved", path);
    assert(!pxa_esp_resource_memory_begin());
    assert(!pxa_esp_assets_begin(config, &backend));
    allocator = pxa_esp_resource_allocator(PXA_MEMORY_EXTERNAL, PXA_MEMORY_TEMPORARY);
    assert(pxa_esp_music_input_acquire("/other/app/zzmusic.ogg", allocator, &input) == PXA_STATUS_DENIED && !input);
    char invalid[1024];
    snprintf(invalid, sizeof(invalid), "%s/assets/../assets/zzmusic.ogg", config->package_root);
    assert(pxa_esp_music_input_acquire(invalid, allocator, &input) != PXA_STATUS_OK && !input);
    snprintf(invalid, sizeof(invalid), "%s/assets/zsound.pcm", config->package_root);
    assert(pxa_esp_music_input_acquire(invalid, allocator, &input) == PXA_STATUS_UNSUPPORTED && !input);
    void *pressure = pxa_memory_allocate(allocator, CONFIG_PXA_RESOURCE_TEMPORARY_EXTERNAL_BYTES -
        (pxa_memory_allocation_bytes(1) - 1));
    assert(pressure && pxa_esp_music_input_acquire(path, allocator, &input) == PXA_STATUS_RESOURCE_LIMIT && !input);
    assert(!g_assets.music_inputs); pxa_memory_release(pressure);
    /* Admission performs no file I/O; a missing file fails on the worker. */
    assert(!rename(path, moved));
    assert(!pxa_esp_music_input_acquire(path, allocator, &input));
    assert(pxa_esp_music_input_open(input, NULL, NULL) == PXA_STATUS_NOT_FOUND);
    pxa_esp_music_input_release(input);
    assert(!rename(moved, path));
    assert(!pxa_esp_music_input_acquire(path, allocator, &input));
    int cancel = 0;
    assert(!pxa_esp_music_input_open(input, &cancel, cancel_music));
    uint8_t data[3000]; size_t count, total = 0;
    while (!pxa_esp_music_input_eof(input)) {
        assert(!pxa_esp_music_input_read(input, data, sizeof(data), &count) && count);
        for (size_t i = 0; i < count; ++i) {
            size_t offset = total + i;
            assert(data[i] == (offset < 12 ? (uint8_t)"OggSOpusHead"[offset] : (offset - 12) % 251));
        }
        total += count;
    }
    assert(total == input->map.info.stored_bytes && input->stream.read_calls == 4);
    assert(!pxa_esp_music_input_read(input, data, sizeof(data), &count) && !count);
    assert(!pxa_esp_music_input_rewind(input));
    assert(!pxa_esp_music_input_read(input, data, 1, &count) && count == 1);
    cancel = 1;
    assert(pxa_esp_music_input_read(input, data, 1, &count) == PXA_STATUS_CANCELLED && !count);
    cancel = 0;
    assert(pxa_esp_music_input_rewind(input) == PXA_STATUS_CANCELLED);
    pxa_esp_music_input_release(input);
    /* A queued input retains catalog storage before its first open. */
    assert(!pxa_esp_music_input_acquire(path, allocator, &input));
    assert(pxa_esp_assets_end() == PXA_STATUS_WOULD_BLOCK);
    assert(pxa_esp_assets_begin(config, &backend) == PXA_STATUS_BUSY);
    assert(pxa_esp_music_input_acquire(path, allocator, &rejected) == PXA_STATUS_BAD_STATE && !rejected);
    assert(pxa_esp_music_input_open(input, NULL, NULL) == PXA_STATUS_CANCELLED);
    pxa_esp_music_input_release(input); end_activation();
    /* Same package, new generation: no stale close/cancel state. */
    assert(!pxa_esp_resource_memory_begin());
    assert(!pxa_esp_assets_begin(config, &backend));
    allocator = pxa_esp_resource_allocator(PXA_MEMORY_EXTERNAL, PXA_MEMORY_TEMPORARY);
    assert(!pxa_esp_music_input_acquire(path, allocator, &input));
    assert(!pxa_esp_music_input_open(input, NULL, NULL));
    assert(!pxa_esp_music_input_read(input, data, 1, &count) && count == 1);
    assert(pxa_esp_assets_end() == PXA_STATUS_WOULD_BLOCK);
    assert(pxa_esp_music_input_read(input, data, 1, &count) == PXA_STATUS_CANCELLED && !count);
    pxa_esp_music_input_release(input); end_activation();
    /* end() must not free the installed catalog while a storage read is blocked.
     * After cancellation, a successfully read block is still not published. */
    assert(!pxa_esp_resource_memory_begin());
    assert(!pxa_esp_assets_begin(config, &backend));
    allocator = pxa_esp_resource_allocator(PXA_MEMORY_EXTERNAL, PXA_MEMORY_TEMPORARY);
    assert(!pxa_esp_music_input_acquire(path, allocator, &input));
    assert(!pxa_esp_music_input_open(input, NULL, NULL));
    music_read_t pending = {0}; pending.input = input;
    pthread_t decoder_thread;
    block_io(); assert(!pthread_create(&decoder_thread, NULL, blocked_music_read, &pending)); wait_io();
    assert(pxa_esp_assets_end() == PXA_STATUS_WOULD_BLOCK);
    resume_io(); assert(!pthread_join(decoder_thread, NULL));
    assert(pending.status == PXA_STATUS_CANCELLED && !pending.count);
    pxa_esp_music_input_release(input); end_activation();
    puts("ESP installed music input: bounds, cancellation, admission budget, deferred shutdown and reactivation passed");
}

/* Exercise real backend readers against one shared device slot. The music
 * fixture tests streaming bytes; codec playback is covered by audio tests. */
static void join_music(pthread_t thread, music_read_t *pending) {
    unsigned i;
    for (i=0;i<3000 && !__atomic_load_n(&pending->done,__ATOMIC_ACQUIRE);++i) vTaskDelay(1);
    assert(i<3000); assert(!pthread_join(thread,NULL));
}
static void check_io_arbitration(const pxa_esp_assets_config_t *config) {
    for (unsigned scenario=0;scenario<4;++scenario) {
        pxa_assets_backend_t backend;
        assert(!pxa_esp_resource_memory_begin());
        assert(!pxa_esp_assets_begin(config,&backend));
        const pxa_memory_allocator_t *allocator=pxa_esp_resource_allocator(PXA_MEMORY_EXTERNAL,PXA_MEMORY_TEMPORARY);
        char path[1024]; snprintf(path,sizeof(path),"%s/assets/zzmusic.ogg",config->package_root);
        pxa_esp_music_input_t *input;
        int cancel=0;
        assert(!pxa_esp_music_input_acquire(path,allocator,&input));
        assert(!pxa_esp_music_input_open(input,&cancel,cancel_music));
        pthread_mutex_lock(&io_mutex); io_trace_enabled=1; io_trace_count=0; pthread_mutex_unlock(&io_mutex);
        block_io();
        pxa_asset_ticket_t ticket;
        assert(!backend.request(NULL,1,bytes("assets/t00.pxr"),1,&ticket)); wait_io();
        music_read_t pending={0}; pending.input=input;
        pthread_t thread; assert(!pthread_create(&thread,NULL,blocked_music_read,&pending));
        pxa_esp_asset_io_stats_t stats;
        unsigned i;
        for (i=0;i<3000;++i) {
            pxa_esp_assets_io_stats(&stats);
            if(stats.waiting[PXA_ESP_ASSET_IO_MUSIC]) break;
            vTaskDelay(1);
        }
        assert(i<3000 && stats.active_lane==PXA_ESP_ASSET_IO_RESOURCE+1);
        pthread_mutex_lock(&io_mutex); assert(io_trace_count==1 && io_trace[0]==PXA_ESP_ASSET_IO_RESOURCE); pthread_mutex_unlock(&io_mutex);
        if (scenario==0) {
            /* A second queued reader cannot replace the registered waiter. */
            pxa_esp_music_input_t *probe;
            assert(!pxa_esp_music_input_acquire(path,allocator,&probe));
            assert(!pxa_esp_music_input_open(probe,NULL,NULL));
            uint8_t data; size_t count=99;
            assert(pxa_esp_music_input_read(probe,&data,1,&count)==PXA_STATUS_WOULD_BLOCK && !count);
            pxa_esp_music_input_release(probe);
            resume_io();
        } else if (scenario==1) {
            __atomic_store_n(&cancel,1,__ATOMIC_RELAXED);
            join_music(thread,&pending);
            assert(pending.status==PXA_STATUS_CANCELLED && !pending.count);
            pxa_esp_assets_io_stats(&stats);
            assert(stats.active_lane==PXA_ESP_ASSET_IO_RESOURCE+1 && !stats.waiting[PXA_ESP_ASSET_IO_MUSIC]);
            assert(stats.lanes[PXA_ESP_ASSET_IO_MUSIC].cancellations==1);
            resume_io();
        } else if (scenario==2) {
            assert(pxa_esp_assets_end()==PXA_STATUS_WOULD_BLOCK);
            /* Both readers must observe closing without releasing the outage. */
            join_music(thread,&pending);
            assert(pending.status==PXA_STATUS_CANCELLED && !pending.count);
            for(i=0;i<3000;++i) {
                pxa_esp_assets_io_stats(&stats);
                if(!stats.active_lane && !stats.waiting[0] && !stats.waiting[1]) break;
                vTaskDelay(1);
            }
            assert(i<3000 && stats.lanes[PXA_ESP_ASSET_IO_RESOURCE].cancellations==1);
            resume_io();
        } else {
            pthread_mutex_lock(&io_mutex); io_fail_once=PXA_STATUS_IO_ERROR; pthread_mutex_unlock(&io_mutex);
            resume_io();
        }
        if(scenario==0 || scenario==3) {
            join_music(thread,&pending);
            assert(!pending.status && pending.count==sizeof(pending.data));
            assert(!memcmp(pending.data,"OggSOpusHead",12));
            pthread_mutex_lock(&io_mutex);
            assert(io_trace_count>=2 && io_trace[1]==PXA_ESP_ASSET_IO_MUSIC);
            pthread_mutex_unlock(&io_mutex);
        }
        if(scenario<2) {
            pxa_raster_asset_t *asset=await_asset(&backend,ticket);
            pxa_raster_asset_release(asset); backend.release(NULL,1,ticket);
        } else if(scenario==3) {
            for(i=0;i<3000;++i) {
                pxa_asset_request_state_t state;
                assert(!backend.query(NULL,1,ticket,&state));
                if(state.state==PXA_ASSET_REQUEST_FAILED) { assert(state.status==PXA_STATUS_IO_ERROR); break; }
                vTaskDelay(1);
            }
            assert(i<3000); backend.release(NULL,1,ticket);
        }
        pxa_esp_music_input_release(input); end_activation();
        pxa_esp_assets_io_stats(&stats);
        assert(!stats.active_lane && !stats.waiting[0] && !stats.waiting[1]);
        assert(stats.lanes[0].max_read_bytes<=4096 && stats.lanes[1].max_read_bytes<=4096);
        if(scenario==3) assert(stats.lanes[0].errors==1);
        pthread_mutex_lock(&io_mutex); io_trace_enabled=0; pthread_mutex_unlock(&io_mutex);
        assert(task_count==1);
    }
    puts("ESP shared storage: music priority, bounded waiters, queued cancellation, outage exit and I/O error recovery passed");
}

static void check_native_images(const pxa_esp_assets_config_t *config) {
    pxa_assets_backend_t backend;
    assert(!pxa_esp_resource_memory_begin());
    assert(!pxa_esp_assets_begin(config,&backend));
    size_t charged=0;
    for(unsigned image=0;image<2;++image) {
        pxa_bytes_t path=bytes(image ? "assets/zzui-rgb.pxr" : "assets/zzui-bgra.pxr");
        pxa_asset_ticket_t a,b;
        block_io(); assert(!backend.request(NULL,1,path,1,&a)); wait_io();
        assert(!backend.request(NULL,1,path,1,&b));
        pxa_asset_object_t *not_ready=NULL;
        assert(backend.acquire(NULL,1,a,&not_ready)==PXA_STATUS_WOULD_BLOCK && !not_ready);
        resume_io();
        pxa_asset_object_t *asset=await_asset(&backend,a), *same=await_asset(&backend,b);
        assert(asset==same);
        pxa_asset_object_view_t view; pxa_asset_object_view(asset,&view);
        assert(view.kind==PXA_ASSET_IMAGE && view.width==320 && view.height==16);
        assert(view.encoding==(image ? PXA_ASSET_ENCODING_RGB565 : PXA_ASSET_ENCODING_BGRA8888));
        assert(view.bytes==320*16*(image ? 2u : 4u));
        if(image) for(size_t i=0;i<320*16;++i) assert(((const uint16_t *)view.data)[i]==0xfc00);
        else for(size_t i=0;i<view.bytes;i+=4) assert(!memcmp(view.data+i,"\x59\x2b\x11\x7b",4));
        charged+=pxa_memory_allocation_bytes(pxa_asset_object_allocation_bytes(asset));
        pxa_memory_stats_t stats; pxa_esp_resource_memory_stats(&stats,NULL);
        assert(stats.by_kind[PXA_MEMORY_IMAGE][1]==charged);
        backend.release(NULL,1,a); backend.release(NULL,1,b);
        pxa_asset_object_release(same); pxa_asset_object_release(asset);
    }
    end_activation();
    puts("ESP UI pixels: existing worker, coalesced loads, RGB565/BGRA alpha, exact IMAGE accounting and clean exit passed");
}

int main(int argc, char **argv) {
    assert(argc == 2);
    /* Deterministic 100 Hz scheduler: quick reads do not pay a 10 ms sleep
     * each; an exhausted CPU slice yields once and starts a new slice. */
    input_t scheduling = {0};
    fake_clock_us = scheduling.last_yield_us = 1000;
    for (unsigned i = 0; i < 16; ++i) { fake_clock_us += 100; input_yield(&scheduling); }
    assert(fake_delays == 0);
    fake_clock_us = 3000; input_yield(&scheduling);
    assert(fake_delays == 1 && scheduling.last_yield_us == 13000);
    input_yield(&scheduling); assert(fake_delays == 1);
    fake_clock_us = 0;
    pxa_package_manifest_t manifest = {0}; pxa_package_file_t files[27];
    char paths[27][64]; uint8_t digests[27][32];
    for (unsigned i = 0; i < 27; ++i) {
        char full[1024]; long size; FILE *f;
        if (i == 0) strcpy(paths[i], "assets/p.pxr");
        else if (i == 1) strcpy(paths[i], "assets/resources.pxi");
        else if (i == 22) strcpy(paths[i], "assets/zmap.bin");
        else if (i == 23) strcpy(paths[i], "assets/zsound.pcm");
        else if (i == 24) strcpy(paths[i], "assets/zzmusic.ogg");
        else if (i == 25) strcpy(paths[i], "assets/zzui-bgra.pxr");
        else if (i == 26) strcpy(paths[i], "assets/zzui-rgb.pxr");
        else snprintf(paths[i], sizeof(paths[i]), "assets/t%02u.pxr", i - 2);
        snprintf(full, sizeof(full), "%s/%s", argv[1], paths[i]); f = fopen(full, "rb"); assert(f);
        assert(!fseek(f, 0, SEEK_END)); size = ftell(f); assert(size > 0); rewind(f);
        uint8_t *data = malloc((size_t)size); assert(fread(data, 1, (size_t)size, f) == (size_t)size); fclose(f);
        assert(!pxa_esp_mbedtls_sha256(data, (size_t)size, digests[i])); free(data);
        files[i] = (pxa_package_file_t){0}; files[i].path = bytes(paths[i]); files[i].size = (uint64_t)size; files[i].sha256 = digests[i];
    }
    manifest.files = files; manifest.file_count = 27; manifest.encoded = bytes("preauthenticated test manifest");
    pxa_esp_assets_config_t config = {0}; pxa_assets_backend_t backend;
    config.manifest = &manifest; config.package_root = argv[1]; config.max_catalog_bytes = 65536;
    config.cache = (pxa_asset_cache_config_t){32, 48, 32, 16, {65536, 524288}, {65536, 524288}};
    config.before_read = before_read; config.notify = notify;
    assert(!pxa_esp_resource_memory_begin());
    assert(!pxa_esp_assets_begin(&config, &backend));
    pxa_assets_backend_t rejected;
    assert(pxa_esp_assets_begin(&config, &rejected) == PXA_STATUS_BUSY);
    assert(backend.request != NULL);
    check_worker_blob_reads(&backend,argv[1],block_io,wait_io,resume_io,blob_delay);
    pxa_asset_ticket_t palette_ticket, tickets[4];
    pxa_asset_ticket_t sound_ticket;
    assert(!backend.request(NULL,1,bytes(paths[23]),1,&sound_ticket));
    pxa_asset_object_t *sound=await_asset(&backend,sound_ticket);
    pxa_asset_request_state_t sound_state;
    assert(!backend.inspect(NULL,1,bytes(paths[23]),&sound_state));
    assert(sound_state.state==PXA_ASSET_REQUEST_READY && !sound_state.status);
    pxa_asset_object_view_t sound_view; pxa_asset_object_view(sound,&sound_view);
    assert(sound_view.kind==PXA_ASSET_AUDIO && sound_view.bytes==160);
    for(unsigned i=0;i<160;++i) assert(sound_view.data[i]==255);
    backend.release(NULL,1,sound_ticket); pxa_asset_object_release_pinned(sound);
    pxa_asset_request_state_t snapshot;
    assert(!backend.inspect(NULL,1,bytes(paths[0]),&snapshot) && snapshot.state == PXA_ASSET_REQUEST_ABSENT);
    assert(!backend.prefetch(NULL, 1, bytes(paths[0]), 0, &palette_ticket));
    pxa_raster_asset_t *palette = await_asset(&backend, palette_ticket);
    assert(!backend.inspect(NULL,1,bytes(paths[0]),&snapshot) && snapshot.state == PXA_ASSET_REQUEST_READY);
    assert(!backend.inspect(NULL,2,bytes(paths[0]),&snapshot) && snapshot.state == PXA_ASSET_REQUEST_ABSENT);
    for (unsigned scene = 0; scene < 100; ++scene) {
        unsigned first = scene % 5 * 4; pxa_raster_bindings_t bindings = {0};
        pxa_raster_bindings_replace(&bindings, 0, palette);
        for (unsigned i = 0; i < 4; ++i) assert(!backend.request(NULL, 1, bytes(paths[first + i + 2]), 1, &tickets[i]));
        for (unsigned i = 0; i < 4; ++i) {
            pxa_raster_asset_t *asset = await_asset(&backend, tickets[i]);
            pxa_raster_bindings_replace(&bindings, (uint8_t)i, asset); pxa_raster_asset_release(asset);
            backend.release(NULL, 1, tickets[i]);
        }
        draw_scene(&bindings, first); pxa_raster_bindings_release(&bindings);
    }
    pxa_asset_cache_stats_t stats; size_t metadata, stack;
    pxa_esp_assets_stats(&stats, &metadata, &stack);
    printf("ESP assets: 100 real-file scenes, peak=%zu B, palette=%zu B, metadata=%zu B, configured task stack=%zu B\n",
        stats.peak_charged[1], stats.peak_charged[0], metadata, stack);
    backend.release(NULL, 1, palette_ticket);
    assert(pxa_esp_assets_end() == PXA_STATUS_WOULD_BLOCK); /* Frame-held palette pins memory. */
    pxa_raster_asset_release(palette); end_activation();
    pxa_esp_assets_stats(&stats, &metadata, &stack);
    assert(metadata == sizeof(g_assets) + sizeof(g_lock_storage) +
                       sizeof(g_lock) + sizeof(g_worker) + sizeof(g_io) + sizeof(g_io_waiting));
    assert(stack == 6144 && stats.peak_charged[0] == 0);

    /* Deterministic read/cancel/retry with one task across activations. */
    assert(!pxa_esp_resource_memory_begin());
    assert(!pxa_esp_assets_begin(&config, &backend)); assert(task_count == 1);
    block_io(); assert(!backend.request(NULL, 1, bytes(paths[2]), 1, &tickets[0])); wait_io();
    backend.release(NULL, 1, tickets[0]);
    assert(!backend.request(NULL, 1, bytes(paths[2]), 1, &tickets[1]));
    resume_io();
    pxa_raster_asset_t *asset = await_asset(&backend, tickets[1]);
    pxa_raster_asset_release(asset); backend.release(NULL, 1, tickets[1]);
    block_io(); assert(!backend.request(NULL, 1, bytes(paths[3]), 1, &tickets[2])); wait_io();
    assert(pxa_esp_assets_end() == PXA_STATUS_WOULD_BLOCK);
    resume_io(); end_activation();
    /* Retire an activation with a blocked blob job; the permanent worker must
     * finish cancellation before package/allocator storage can be reused. */
    assert(!pxa_esp_resource_memory_begin());
    assert(!pxa_esp_assets_begin(&config,&backend));
    block_io(); uint64_t read_ticket=begin_blob(&backend,0,PXA_ASSET_READ_MAX_BYTES,blob_delay); wait_io();
    assert(pxa_esp_assets_end()==PXA_STATUS_WOULD_BLOCK);
    resume_io(); end_activation();
    assert(!pxa_esp_resource_memory_begin());
    assert(!pxa_esp_assets_begin(&config,&backend));
    void *read_pressure=pxa_esp_resource_allocate(PXA_MEMORY_EXTERNAL,PXA_MEMORY_TEMPORARY,
        CONFIG_PXA_RESOURCE_TEMPORARY_EXTERNAL_BYTES-(pxa_memory_allocation_bytes(1)-1));
    assert(read_pressure);
    read_ticket=begin_blob(&backend,0,PXA_ASSET_READ_MAX_BYTES,blob_delay); pxa_bytes_t blob;
    assert(await_blob(&backend,read_ticket,&blob,blob_delay)==PXA_STATUS_RESOURCE_LIMIT);
    backend.read_release(NULL,1,read_ticket); pxa_memory_release(read_pressure);
    read_ticket=begin_blob(&backend,4097,PXA_ASSET_READ_MAX_BYTES,blob_delay);
    check_blob_result(&backend,read_ticket,4097,PXA_ASSET_READ_MAX_BYTES,blob_delay); end_activation();
    unsigned tries;
    /* Resident image storage can exhaust the shared budget even when the file
     * cache has plenty of room. TEMPORARY deliberately cannot fill the whole
     * global budget now. A failed load must recover after image release. */
    assert(!pxa_esp_resource_memory_begin());
    assert(!pxa_esp_assets_begin(&config, &backend));
    pxa_memory_stats_t memory_stats;
    pxa_esp_resource_memory_stats(&memory_stats, NULL);
    size_t fill_size = CONFIG_PXA_RESOURCE_EXTERNAL_BYTES - memory_stats.charged[1] -
        32768 - (pxa_memory_allocation_bytes(1) - 1);
    void *pressure = pxa_esp_resource_allocate(PXA_MEMORY_EXTERNAL, PXA_MEMORY_IMAGE, fill_size);
    assert(pressure);
    assert(!backend.request(NULL, 1, bytes(paths[2]), 1, &tickets[0]));
    for (tries = 0; tries < 15000; ++tries) {
        pxa_asset_request_state_t state;
        assert(!backend.query(NULL, 1, tickets[0], &state));
        if (state.state == PXA_ASSET_REQUEST_FAILED) {
            assert(state.status == PXA_STATUS_RESOURCE_LIMIT); break;
        }
        assert(state.state == PXA_ASSET_REQUEST_QUEUED || state.state == PXA_ASSET_REQUEST_LOADING);
        vTaskDelay(1);
    }
    assert(tries < 15000);
    backend.release(NULL, 1, tickets[0]);
    pxa_memory_release(pressure);
    assert(!backend.request(NULL, 1, bytes(paths[2]), 1, &tickets[1]));
    asset = await_asset(&backend, tickets[1]);
    pxa_raster_asset_release(asset); backend.release(NULL, 1, tickets[1]);
    end_activation();
    pxa_esp_resource_memory_stats(&memory_stats, NULL);
    assert(!memory_stats.charged[0] && !memory_stats.charged[1] && memory_stats.denied);
    assert(memory_stats.peak[1] <= CONFIG_PXA_RESOURCE_EXTERNAL_BYTES);
    /* Cache has spare local quota, but another resource needs its idle bytes. */
    assert(!pxa_esp_resource_memory_begin());
    assert(!pxa_esp_assets_begin(&config, &backend));
    assert(!backend.request(NULL, 1, bytes(paths[2]), 1, &tickets[0]));
    asset = await_asset(&backend, tickets[0]);
    pxa_raster_asset_release(asset); backend.release(NULL, 1, tickets[0]);
    pxa_esp_resource_memory_stats(&memory_stats, NULL);
    size_t metadata_charge = memory_stats.by_kind[PXA_MEMORY_METADATA][1];
    fill_size = CONFIG_PXA_RESOURCE_EXTERNAL_BYTES - metadata_charge -
        (pxa_memory_allocation_bytes(1) - 1) - 1024;
    assert(!pxa_esp_resource_allocate(PXA_MEMORY_EXTERNAL, PXA_MEMORY_IMAGE, fill_size));
    for (tries = 0; tries < 15000; ++tries) {
        pxa_esp_resource_memory_stats(&memory_stats, NULL);
        if (!memory_stats.by_kind[PXA_MEMORY_RASTER][1]) break;
        vTaskDelay(1);
    }
    assert(tries < 15000);
    pressure = pxa_esp_resource_allocate(PXA_MEMORY_EXTERNAL, PXA_MEMORY_IMAGE, fill_size);
    assert(pressure); pxa_memory_release(pressure); end_activation();
    puts("ESP shared budget: external pressure, load failure rollback and retry recovery passed");
    check_native_images(&config);
    check_music_inputs(&config);
    check_io_arbitration(&config);
    assert(task_count == 1 && __atomic_load_n(&notices, __ATOMIC_RELAXED));
    pthread_mutex_lock(&task_mutex); task_exit = 1; pthread_cond_signal(&task_condition); pthread_mutex_unlock(&task_mutex);
    assert(!pthread_join(task_thread, NULL));
    puts("ESP assets: cancelled reads, frame-held shutdown, repeated activation and zero leaked allocations passed");
    return 0;
}
