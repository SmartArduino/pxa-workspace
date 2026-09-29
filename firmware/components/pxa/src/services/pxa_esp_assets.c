#include "pxa_esp_assets.h"
#include "pxa_esp_resource_memory.h"
#include "pxa/asset_stream.h"

#if defined(ESP_PLATFORM)
#include "sdkconfig.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef CONFIG_PXA_ASSET_WORKER_STACK_SIZE
#define CONFIG_PXA_ASSET_WORKER_STACK_SIZE 6144
#endif
#define ASSET_ROOT_MAX 255u
#define ASSET_WORKER_PRIORITY 3u
#define ASSET_WORK_SLICE_US 2000

typedef struct {
    pxa_esp_assets_config_t config;
    pxa_asset_catalog_t catalog;
    pxa_asset_cache_t *cache;
    void *workspace;
    uint8_t *index;
    uint8_t package_key[32];
    char root[ASSET_ROOT_MAX + 1];
    size_t metadata_bytes;
    uint32_t music_inputs;
    uint8_t closing;
    uint8_t last_read;
    pxa_asset_read_queue_t reads;
} activation_t;

/* Runtime-thread begin/end calls are serialized. Worker/cache callbacks use
 * this short mutex. Static state and task outlive every activation. */
static activation_t g_assets;
static StaticSemaphore_t g_lock_storage;
static SemaphoreHandle_t g_lock;
static TaskHandle_t g_worker;
static pxa_esp_asset_io_stats_t g_io;
static TaskHandle_t g_io_waiting[2];

static void lock(void) { (void)xSemaphoreTake(g_lock, portMAX_DELAY); }
static void unlock(void) { (void)xSemaphoreGive(g_lock); }
static void wake(void) { if (g_worker) xTaskNotifyGive(g_worker); }

typedef struct {
    int fd;
    uint64_t expected, consumed, job, position;
    int64_t last_yield_us;
    uint8_t blob;
    pxa_esp_music_input_t *music; /* Borrowed during this input's read only. */
} input_t;

static int music_request_cancelled(const pxa_esp_music_input_t *input);
static int input_cancelled_locked(const input_t *input) {
    return g_assets.closing || (input->job && (input->blob ?
        pxa_asset_read_cancelled(&g_assets.reads,input->job) :
        pxa_asset_cache_job_cancelled(g_assets.cache,input->job)));
}
static int input_cancelled(void *context) {
    input_t *input=context;
    lock(); int cancelled=input_cancelled_locked(input); unlock();
    return cancelled || (input->music && music_request_cancelled(input->music));
}
/* Kernel notification is nonblocking. Keep the short state lock through the
 * give so a cancelling reader cannot unregister/retire its task concurrently.
 * No caller callback, file operation or allocation is invoked with this lock. */
static void wake_io_locked(void) {
    unsigned next=g_io_waiting[PXA_ESP_ASSET_IO_MUSIC] ? PXA_ESP_ASSET_IO_MUSIC : PXA_ESP_ASSET_IO_RESOURCE;
    if (!g_io.active_lane && g_io_waiting[next]) xTaskNotifyGive(g_io_waiting[next]);
}
static pxa_status_t input_read(void *context,uint8_t *output,size_t capacity,size_t *count) {
    input_t *input=context;
    if (!count || !output || capacity>PXA_ASSET_READ_CHUNK_BYTES) return PXA_STATUS_INVALID_ARGUMENT;
    *count=0;
    const unsigned lane=input->music ? PXA_ESP_ASSET_IO_MUSIC : PXA_ESP_ASSET_IO_RESOURCE;
    const uint64_t started=esp_timer_get_time();
    TaskHandle_t self=xTaskGetCurrentTaskHandle();
    lock();
    if (g_io_waiting[lane]) { unlock(); return PXA_STATUS_WOULD_BLOCK; }
    g_io_waiting[lane]=self; g_io.waiting[lane]=1;
    unlock();
    for (;;) {
        /* An audio cancellation callback may acquire the audio mutex. */
        int cancelled=input->music && music_request_cancelled(input->music);
        lock();
        if (cancelled || input_cancelled_locked(input)) {
            g_io_waiting[lane]=NULL; g_io.waiting[lane]=0;
            ++g_io.lanes[lane].cancellations;
            wake_io_locked(); unlock(); return PXA_STATUS_CANCELLED;
        }
        if (!g_io.active_lane && (lane==PXA_ESP_ASSET_IO_MUSIC || !g_io_waiting[PXA_ESP_ASSET_IO_MUSIC])) {
            g_io.active_lane=(uint8_t)(lane+1);
            g_io_waiting[lane]=NULL; g_io.waiting[lane]=0;
            unlock(); break;
        }
        unlock();
        /* Reuse the reader task's wakeup. Its outer job/command loop always
         * rechecks its queue, so consuming a coalesced wake loses no work. */
        TickType_t ticks=pdMS_TO_TICKS(10);
        (void)ulTaskNotifyTake(pdTRUE,ticks ? ticks : 1);
    }
    const uint64_t granted=esp_timer_get_time();
    pxa_status_t status=PXA_STATUS_OK;
    ssize_t n=0; unsigned issued=0;
    if (g_assets.config.before_read)
        status=g_assets.config.before_read(g_assets.config.io_context,lane,capacity,input,input_cancelled);
    if (!status && input_cancelled(input)) status=PXA_STATUS_CANCELLED;
    if (!status) {
        issued=1;
        do { n=read(input->fd,output,capacity); } while (n<0 && errno==EINTR);
        if (n<0) status=PXA_STATUS_IO_ERROR;
        else if ((uint64_t)n>input->expected-input->consumed) status=PXA_STATUS_PROTOCOL_ERROR;
    }
    const uint64_t ended=esp_timer_get_time();
    lock();
    pxa_esp_asset_io_lane_t *stats=&g_io.lanes[lane];
    stats->reads+=issued; if (n>0) stats->bytes+=(uint64_t)n;
    stats->wait_us+=granted-started; stats->service_us+=ended-granted;
    if (stats->max_wait_us<granted-started) stats->max_wait_us=granted-started;
    if (stats->max_service_us<ended-granted) stats->max_service_us=ended-granted;
    if (stats->max_read_bytes<capacity) stats->max_read_bytes=(uint32_t)capacity;
    stats->cancellations+=status==PXA_STATUS_CANCELLED;
    stats->errors+=status && status!=PXA_STATUS_CANCELLED;
    g_io.active_lane=0; wake_io_locked(); unlock();
    if (status) return status;
    input->consumed+=(size_t)n; input->position+=(size_t)n; *count=(size_t)n;
    return PXA_STATUS_OK;
}
static void input_yield(void *context) {
    input_t *input = context;
    /* Audio decoder/output preempt this lower-priority task. Give idle/lower
     * priorities time too, but do not impose a whole RTOS tick per 32-byte
     * header or 4 KiB read. Check the time budget between bounded blocks. */
    if (esp_timer_get_time() - input->last_yield_us >= ASSET_WORK_SLICE_US) {
        vTaskDelay(1);
        input->last_yield_us = esp_timer_get_time();
    }
}
static void input_close(input_t *input) {
    if (input->fd >= 0) close(input->fd);
    input->fd = -1;
}
static pxa_status_t input_open(input_t *input, pxa_bytes_t path, uint64_t bytes, uint64_t job) {
    char full[ASSET_ROOT_MAX + 1 + PXA_ASSET_PATH_MAX + 1];
    struct stat metadata;
    int flags = O_RDONLY;
    memset(input, 0, sizeof(*input)); input->fd = -1;
    input->last_yield_us = esp_timer_get_time();
    if (!pxa_asset_path_valid(path)) return PXA_STATUS_DENIED;
    int n = snprintf(full, sizeof(full), "%s/%.*s", g_assets.root, (int)path.size, path.data);
    if (n < 0 || (size_t)n >= sizeof(full)) return PXA_STATUS_LIMIT_EXCEEDED;
    /* ESP's Host-owned LittleFS has no symlinks/device nodes. The root is a
     * verified package directory; normalized index paths cannot escape it.
     * O_NOFOLLOW also rejects leaf symlinks when host-testing this adapter. */
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
#ifdef O_NONBLOCK
    flags |= O_NONBLOCK;
#endif
    input->fd = open(full, flags);
    if (input->fd < 0) return errno == ENOENT ? PXA_STATUS_NOT_FOUND : PXA_STATUS_DENIED;
    if (fstat(input->fd, &metadata) || !S_ISREG(metadata.st_mode) || metadata.st_size < 0 ||
        (uint64_t)metadata.st_size != bytes) {
        input_close(input); return PXA_STATUS_DENIED;
    }
    input->expected = bytes; input->job = job;
    return PXA_STATUS_OK;
}
struct pxa_esp_music_input {
    pxa_asset_block_map_t map;
    pxa_asset_stream_t stream;
    input_t file;
    void *cancel_context;
    int (*cancelled)(void *);
    uint8_t opened;
};

static int music_request_cancelled(const pxa_esp_music_input_t *input) {
    return input->cancelled && input->cancelled(input->cancel_context);
}
static void music_unpin(void) {
    void (*notify)(void *) = NULL;
    void *context = NULL;
    lock();
    if (--g_assets.music_inputs == 0 && g_assets.closing) {
        notify = g_assets.config.notify;
        context = g_assets.config.notify_context;
    }
    unlock();
    if (notify) notify(context);
}
pxa_status_t pxa_esp_music_input_acquire(const char *path,
    const pxa_memory_allocator_t *allocator, pxa_esp_music_input_t **out) {
    if (!out) return PXA_STATUS_INVALID_ARGUMENT;
    *out = NULL;
    if (!path || !allocator) return PXA_STATUS_INVALID_ARGUMENT;
    if (!g_lock) return PXA_STATUS_BAD_STATE;
    pxa_asset_block_map_t map;
    lock();
    if (!g_assets.cache || g_assets.closing) { unlock(); return PXA_STATUS_BAD_STATE; }
    size_t root_bytes = strlen(g_assets.root);
    size_t path_bytes = strnlen(path, ASSET_ROOT_MAX + PXA_ASSET_PATH_MAX + 2);
    if (path_bytes <= root_bytes + 1 || path_bytes > root_bytes + 1 + PXA_ASSET_PATH_MAX ||
        memcmp(path, g_assets.root, root_bytes) || path[root_bytes] != '/') {
        unlock(); return PXA_STATUS_DENIED;
    }
    pxa_bytes_t relative = {(const uint8_t *)path + root_bytes + 1, path_bytes - root_bytes - 1};
    pxa_status_t status = pxa_asset_catalog_block_map(&g_assets.catalog, relative, &map);
    if (!status && (map.info.kind != PXA_ASSET_AUDIO ||
        (map.info.encoding != PXA_ASSET_ENCODING_OGG_VORBIS &&
         map.info.encoding != PXA_ASSET_ENCODING_OGG_OPUS))) status = PXA_STATUS_UNSUPPORTED;
    if (!status && g_assets.music_inputs == UINT32_MAX) status = PXA_STATUS_RESOURCE_LIMIT;
    if (!status) ++g_assets.music_inputs;
    unlock();
    if (status) return status;
    pxa_esp_music_input_t *input = pxa_memory_allocate(allocator, sizeof(*input));
    if (!input) { music_unpin(); return PXA_STATUS_RESOURCE_LIMIT; }
    memset(input, 0, sizeof(*input));
    input->file.fd = -1;
    input->map = map;
    *out = input;
    return PXA_STATUS_OK;
}
static int music_cancelled(void *context) {
    pxa_esp_music_input_t *input = context;
    return input_cancelled(&input->file) ||
        (!input->file.music && music_request_cancelled(input));
}
static pxa_status_t music_load(void *context, const pxa_asset_block_t *block,
    uint8_t *buffer, size_t capacity) {
    pxa_esp_music_input_t *input = context;
    if ((uint64_t)(off_t)block->offset != block->offset ||
        (input->file.position != block->offset && lseek(input->file.fd, (off_t)block->offset, SEEK_SET) < 0)) return PXA_STATUS_IO_ERROR;
    input->file.position = block->offset;
    input->file.expected = block->bytes;
    input->file.consumed = 0;
    pxa_asset_input_t callbacks = {&input->file, input_read, input_cancelled, input_yield};
    return pxa_asset_load_block(block, &callbacks, buffer, capacity);
}
pxa_status_t pxa_esp_music_input_open(pxa_esp_music_input_t *input,
    void *context, int (*cancelled)(void *)) {
    if (!input || input->opened) return PXA_STATUS_BAD_STATE;
    input->cancel_context = context; input->cancelled = cancelled;
    if (music_cancelled(input)) return PXA_STATUS_CANCELLED;
    pxa_status_t status = input_open(&input->file, input->map.info.path,
        input->map.info.stored_bytes, 0);
    if (!status) input->file.music=input;
    if (!status) status = pxa_asset_stream_init(&input->stream, &input->map,
        input, music_load, music_cancelled);
    if (!status) input->opened = 1;
    else input_close(&input->file);
    return status;
}
pxa_status_t pxa_esp_music_input_read(pxa_esp_music_input_t *input,
    uint8_t *output, size_t capacity, size_t *bytes) {
    if (bytes) *bytes = 0;
    if (!input || !input->opened) return PXA_STATUS_BAD_STATE;
    return pxa_asset_stream_read(&input->stream, output, capacity, bytes);
}
pxa_status_t pxa_esp_music_input_rewind(pxa_esp_music_input_t *input) {
    if (!input || !input->opened) return PXA_STATUS_BAD_STATE;
    return pxa_asset_stream_seek(&input->stream, 0);
}
int pxa_esp_music_input_eof(const pxa_esp_music_input_t *input) {
    return input && input->opened && input->stream.position == input->map.info.stored_bytes;
}
void pxa_esp_music_input_release(pxa_esp_music_input_t *input) {
    if (!input) return;
    input_close(&input->file);
    pxa_memory_release(input);
    music_unpin();
}

static void *pixel_allocate(void *context, size_t bytes) {
    return pxa_memory_allocate(context, bytes);
}
static void pixel_release(void *context, void *memory) { (void)context; pxa_memory_release(memory); }
static void release_job(const pxa_asset_job_t *job) {
    pxa_raster_asset_release(job->asset);
    lock();
    (void)pxa_asset_cache_finish_release(g_assets.cache, job->token);
    unlock();
}
static void run_worker(void *context) {
    (void)context;
    for (;;) {
        pxa_asset_job_t job, discard;
        pxa_asset_read_job_t read_job = {0};
        pxa_status_t next;
        void (*notify)(void *);
        void *notify_context;
        lock();
        next = PXA_STATUS_NOT_FOUND;
        if (g_assets.cache) {
            if (!g_assets.last_read) next = pxa_asset_read_next(&g_assets.reads,&read_job);
            if (next != PXA_STATUS_OK) next = pxa_asset_cache_next_job(g_assets.cache,&job);
            if (next != PXA_STATUS_OK) next = pxa_asset_read_next(&g_assets.reads,&read_job);
            if (next == PXA_STATUS_OK) g_assets.last_read = read_job.kind != 0;
        }
        notify = g_assets.config.notify;
        notify_context = g_assets.config.notify_context;
        unlock();
        if (next != PXA_STATUS_OK) { (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY); continue; }
        if (read_job.kind == PXA_ASSET_READ_JOB_RELEASE) {
            pxa_memory_release(read_job.buffer);
            lock(); pxa_asset_read_freed(&g_assets.reads,read_job.token); unlock();
        } else if (read_job.kind == PXA_ASSET_READ_JOB_LOAD) {
            input_t input;
            uint8_t *buffer = NULL; size_t bytes = 0;
            pxa_status_t status = input_open(&input,read_job.block.info.path,
                read_job.block.info.stored_bytes,read_job.token);
            if (!status) {
                if ((uint64_t)(off_t)read_job.block.offset != read_job.block.offset ||
                    lseek(input.fd,(off_t)read_job.block.offset,SEEK_SET) < 0) status = PXA_STATUS_IO_ERROR;
                else {
                    input.expected = read_job.block.bytes; input.blob = 1;
                    pxa_asset_input_t callbacks = {&input,input_read,input_cancelled,input_yield};
                    status = pxa_asset_read_load(&read_job,&callbacks,g_assets.reads.allocator,&buffer,&bytes);
                }
                input_close(&input);
            }
            lock(); pxa_asset_read_finish(&g_assets.reads,read_job.token,status,buffer,bytes); unlock();
        } else if (job.kind == PXA_ASSET_JOB_RELEASE) release_job(&job);
        else if (job.kind == PXA_ASSET_JOB_LOAD) {
            input_t input;
            pxa_raster_asset_t *asset = NULL;
            pxa_status_t status = input_open(&input, job.info.path, job.info.stored_bytes, job.token);
            if (status == PXA_STATUS_OK) {
                pxa_asset_input_t callbacks = {&input, input_read, input_cancelled, input_yield};
                status = pxa_asset_load_resident(&job.info, &callbacks, pixel_allocate, pixel_release,
                    (void *)pxa_esp_resource_allocator(job.memory_class, job.info.kind == PXA_ASSET_AUDIO ? PXA_MEMORY_AUDIO :
                        job.info.kind == PXA_ASSET_IMAGE ? PXA_MEMORY_IMAGE : PXA_MEMORY_RASTER), &asset);
                input_close(&input);
            }
            lock();
            (void)pxa_asset_cache_finish_load(g_assets.cache, job.token, status, asset, &discard);
            unlock();
            if (discard.kind == PXA_ASSET_JOB_RELEASE) release_job(&discard);
        }
        /* Captured callback targets persistent Host state, never an app arena.
         * No activation state is touched after finish_load/finish_release. */
        if (notify) notify(notify_context);
    }
}

static pxa_status_t find_asset(void *context, pxa_component_t component, pxa_bytes_t path, pxa_asset_info_t *info) {
    (void)context; (void)component;
    /* Calls and begin/end are serialized by the runtime thread. */
    if (!g_assets.cache || g_assets.closing) return PXA_STATUS_BAD_STATE;
    return pxa_asset_catalog_find(&g_assets.catalog, path, info);
}
static pxa_status_t request_asset_common(void *context, pxa_component_t component, pxa_bytes_t path,
    uint8_t cls, pxa_asset_ticket_t *ticket, int prefetch) {
    pxa_asset_info_t info;
    pxa_status_t status = find_asset(context, component, path, &info);
    if (status != PXA_STATUS_OK) return status;
    lock();
    status = (prefetch ? pxa_asset_cache_prefetch : pxa_asset_cache_request)(
        g_assets.cache, component, g_assets.package_key, &info, cls, ticket);
    unlock(); wake(); return status;
}
static pxa_status_t request_asset(void *context, pxa_component_t component, pxa_bytes_t path,
    uint8_t cls, pxa_asset_ticket_t *ticket) {
    return request_asset_common(context, component, path, cls, ticket, 0);
}
static pxa_status_t prefetch_asset(void *context, pxa_component_t component, pxa_bytes_t path,
    uint8_t cls, pxa_asset_ticket_t *ticket) {
    return request_asset_common(context, component, path, cls, ticket, 1);
}
static pxa_status_t inspect_asset(void *context, pxa_component_t component, pxa_bytes_t path,
    pxa_asset_request_state_t *state) {
    pxa_asset_info_t info;
    if (!state) return PXA_STATUS_INVALID_ARGUMENT;
    memset(state, 0, sizeof(*state));
    pxa_status_t status = find_asset(context, component, path, &info);
    if (status != PXA_STATUS_OK) return status;
    if (!pxa_asset_object_required_bytes(&info)) return PXA_STATUS_UNSUPPORTED;
    lock();
    status = pxa_asset_cache_inspect(g_assets.cache, component, g_assets.package_key, &info,
        pxa_asset_default_memory_class(&info), state);
    unlock(); return status;
}
static pxa_status_t query_asset(void *context, pxa_component_t component, pxa_asset_ticket_t ticket,
    pxa_asset_request_state_t *state) {
    pxa_status_t status; (void)context;
    lock(); status = pxa_asset_cache_query(g_assets.cache, component, ticket, state); unlock(); return status;
}
static pxa_status_t acquire_asset(void *context, pxa_component_t component, pxa_asset_ticket_t ticket,
    pxa_raster_asset_t **asset) {
    pxa_status_t status; (void)context;
    lock(); status = pxa_asset_cache_acquire(g_assets.cache, component, ticket, asset); unlock(); return status;
}
static void release_asset(void *context, pxa_component_t component, pxa_asset_ticket_t ticket) {
    (void)context;
    lock(); (void)pxa_asset_cache_release(g_assets.cache, component, ticket); unlock(); wake();
}
static pxa_status_t read_asset(void *context, pxa_component_t component, pxa_bytes_t path,
    uint32_t offset, uint32_t bytes, uint64_t *ticket) {
    (void)context;
    if (!ticket) return PXA_STATUS_INVALID_ARGUMENT;
    *ticket = 0;
    if (!g_assets.cache || g_assets.closing) return PXA_STATUS_BAD_STATE;
    pxa_asset_blob_block_t block;
    pxa_status_t status = pxa_asset_catalog_blob_block(&g_assets.catalog,path,offset,&block);
    if (status) return status;
    lock(); status = pxa_asset_read_begin(&g_assets.reads,component,&block,offset,bytes,ticket); unlock();
    wake(); return status;
}
static pxa_status_t read_asset_result(void *context, pxa_component_t component, uint64_t ticket, pxa_bytes_t *result) {
    (void)context;
    lock(); pxa_status_t status = pxa_asset_read_result(&g_assets.reads,component,ticket,result); unlock();
    return status;
}
static void read_asset_release(void *context, pxa_component_t component, uint64_t ticket) {
    (void)context;
    lock(); (void)pxa_asset_read_release(&g_assets.reads,component,ticket); unlock(); wake();
}
static pxa_status_t load_catalog(void) {
    pxa_bytes_t path = {(const uint8_t *)PXA_ASSET_INDEX_PATH, sizeof(PXA_ASSET_INDEX_PATH) - 1};
    const pxa_package_file_t *file = pxa_package_file_find(g_assets.config.manifest, path);
    size_t offset = 0, count;
    uint8_t extra;
    input_t input;
    pxa_status_t status;
    if (!file) return PXA_STATUS_NOT_FOUND;
    if (file->size < PXA_ASSET_INDEX_HEADER_BYTES || file->size > g_assets.config.max_catalog_bytes)
        return PXA_STATUS_RESOURCE_LIMIT;
    g_assets.index = pxa_esp_resource_allocate(PXA_MEMORY_EXTERNAL, PXA_MEMORY_METADATA, (size_t)file->size);
    if (!g_assets.index) return PXA_STATUS_RESOURCE_LIMIT;
    g_assets.metadata_bytes += (size_t)file->size;
    status = input_open(&input, path, file->size, 0);
    if (status != PXA_STATUS_OK) return status;
    while (offset < file->size) {
        size_t capacity = (size_t)file->size - offset;
        if (capacity > PXA_ASSET_READ_CHUNK_BYTES) capacity = PXA_ASSET_READ_CHUNK_BYTES;
        status = input_read(&input, g_assets.index + offset, capacity, &count);
        if (status != PXA_STATUS_OK || !count) { if (status == PXA_STATUS_OK) status = PXA_STATUS_IO_ERROR; break; }
        offset += count;
        input_yield(&input);
    }
    if (status == PXA_STATUS_OK) {
        status = input_read(&input, &extra, 1, &count);
        if (status == PXA_STATUS_OK && count) status = PXA_STATUS_PROTOCOL_ERROR;
    }
    input_close(&input);
    if (status != PXA_STATUS_OK) return status;
    return pxa_asset_catalog_init(&g_assets.catalog,
        (pxa_bytes_t){g_assets.index, (size_t)file->size}, g_assets.config.manifest);
}

pxa_status_t pxa_esp_assets_begin(const pxa_esp_assets_config_t *config, pxa_assets_backend_t *backend) {
    pxa_status_t status;
    pxa_asset_cache_t *cache;
    size_t bytes;
    if (!config || !backend || !config->manifest || !config->package_root ||
        !config->package_root[0] || strlen(config->package_root) > ASSET_ROOT_MAX ||
        !config->manifest->encoded.data || !config->manifest->encoded.size ||
        !config->max_catalog_bytes || !(bytes = pxa_asset_cache_workspace_size(&config->cache)))
        return PXA_STATUS_INVALID_ARGUMENT;
    memset(backend, 0, sizeof(*backend));
    if (!g_lock) g_lock = xSemaphoreCreateMutexStatic(&g_lock_storage);
    if (!g_lock) return PXA_STATUS_RESOURCE_LIMIT;
    lock();
    if (g_assets.cache || g_assets.workspace || g_io.active_lane || g_io_waiting[0] || g_io_waiting[1]) { unlock(); return PXA_STATUS_BUSY; }
    memset(&g_io,0,sizeof(g_io));
    g_assets.config = *config;
    strcpy(g_assets.root, config->package_root);
    g_assets.metadata_bytes = bytes;
    unlock();
    g_assets.workspace = pxa_esp_resource_allocate(PXA_MEMORY_EXTERNAL, PXA_MEMORY_METADATA, bytes);
    /* Activation-private cache: namespace need not rehash the installed manifest. */
    status = g_assets.workspace ? PXA_STATUS_OK : PXA_STATUS_RESOURCE_LIMIT;
    if (status == PXA_STATUS_OK) status = load_catalog();
    if (status == PXA_STATUS_OK) status = pxa_asset_cache_init(g_assets.workspace, bytes, &config->cache, &cache);
    if (status == PXA_STATUS_OK) pxa_asset_read_init(&g_assets.reads,
        pxa_esp_resource_allocator(PXA_MEMORY_EXTERNAL,PXA_MEMORY_TEMPORARY));
    if (status == PXA_STATUS_OK && !g_worker && xTaskCreateWithCaps(run_worker, "pxa_assets",
        CONFIG_PXA_ASSET_WORKER_STACK_SIZE, NULL, ASSET_WORKER_PRIORITY, &g_worker,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) status = PXA_STATUS_RESOURCE_LIMIT;
    if (status == PXA_STATUS_OK) {
        lock(); g_assets.cache = cache; unlock();
        *backend = (pxa_assets_backend_t){NULL, find_asset, request_asset, query_asset, acquire_asset, release_asset, prefetch_asset, inspect_asset,
            read_asset,read_asset_result,read_asset_release};
        return PXA_STATUS_OK;
    }
    void *workspace = g_assets.workspace, *index = g_assets.index;
    lock(); memset(&g_assets, 0, sizeof(g_assets)); unlock();
    pxa_memory_release(index); pxa_memory_release(workspace);
    return status;
}

pxa_status_t pxa_esp_assets_end(void) {
    void *workspace, *index;
    if (!g_lock) return PXA_STATUS_OK;
    lock();
    if (!g_assets.cache) { unlock(); return PXA_STATUS_OK; }
    g_assets.closing = 1;
    for (unsigned lane=0;lane<2;++lane) if (g_io_waiting[lane]) xTaskNotifyGive(g_io_waiting[lane]);
    pxa_asset_cache_shutdown(g_assets.cache);
    pxa_asset_read_shutdown(&g_assets.reads);
    if (g_assets.music_inputs || g_io.active_lane || g_io_waiting[0] || g_io_waiting[1] || !pxa_asset_cache_drained(g_assets.cache) || !pxa_asset_read_drained(&g_assets.reads)) {
        unlock(); wake(); return PXA_STATUS_WOULD_BLOCK;
    }
    workspace = g_assets.workspace; index = g_assets.index;
    memset(&g_assets, 0, sizeof(g_assets));
    unlock();
    pxa_memory_release(index); pxa_memory_release(workspace);
    return PXA_STATUS_OK;
}
size_t pxa_esp_assets_trim(uint8_t cls, size_t needed) {
    if (!g_lock) return 0;
    lock();
    size_t planned = pxa_asset_cache_trim(g_assets.cache, cls, needed);
    unlock();
    if (planned) wake();
    return planned;
}
void pxa_esp_assets_io_stats(pxa_esp_asset_io_stats_t *stats) {
    if (!stats) return;
    memset(stats,0,sizeof(*stats));
    if (!g_lock) return;
    lock(); *stats=g_io; unlock();
}
void pxa_esp_assets_stats(pxa_asset_cache_stats_t *stats, size_t *metadata_bytes, size_t *task_stack_bytes) {
    if (stats) memset(stats, 0, sizeof(*stats));
    const size_t fixed_metadata = sizeof(g_assets) + sizeof(g_lock_storage) +
                                  sizeof(g_lock) + sizeof(g_worker) + sizeof(g_io) + sizeof(g_io_waiting);
    if (metadata_bytes) *metadata_bytes = fixed_metadata;
    if (task_stack_bytes) *task_stack_bytes = g_worker ? CONFIG_PXA_ASSET_WORKER_STACK_SIZE : 0;
    if (!g_lock) return;
    lock();
    if (g_assets.cache && stats) pxa_asset_cache_stats(g_assets.cache, stats);
    if (metadata_bytes) *metadata_bytes += g_assets.metadata_bytes;
    unlock();
}
#endif
