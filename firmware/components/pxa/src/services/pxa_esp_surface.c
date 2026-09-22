#include "pxa/pxa_esp_surface.h"

#if defined(ESP_PLATFORM)

#include <string.h>

#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#if CONFIG_PXA_PARALLEL_RASTER
#include "esp_attr.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#endif
#include "pxa/wire.h"
#include "sdkconfig.h"

#define PXA_ESP_SURFACE_MAX_BUFFERS 3
#define PXA_ESP_SURFACE_NONE (-1)
#define PXA_ESP_SURFACE_MAGIC UINT32_C(0x45504753)
#define PXA_ESP_SURFACE_LATENCY_BUCKETS 64u
#define PXA_ESP_RASTER_MAILBOX_SLOTS 2u
#define PXA_ESP_RASTER_MAILBOX_MIN_BYTES UINT32_C(4096)
#define PXA_ESP_RASTER_MAILBOX_GROW_BYTES UINT32_C(4096)
#define PXA_ESP_SURFACE_FLAG_GAME_RENDER UINT8_C(8)

static const char *const PXA_ESP_SURFACE_TAG = "PxaSurface";

#ifndef PXA_ESP_SURFACE_GUEST_MAPPING_SUPPORTED
#if defined(CONFIG_WAMR_LINEAR_MEMORY_RESERVE_MAX) && \
    CONFIG_WAMR_LINEAR_MEMORY_RESERVE_MAX
#define PXA_ESP_SURFACE_GUEST_MAPPING_SUPPORTED 1
#else
#define PXA_ESP_SURFACE_GUEST_MAPPING_SUPPORTED 0
#endif
#endif

typedef struct {
    uint32_t magic;
    uint8_t *buffers[PXA_ESP_SURFACE_MAX_BUFFERS];
    uint32_t frame_bytes;
    uint32_t stride_bytes;
    uint16_t width;
    uint16_t height;
    uint16_t format;
    uint8_t flags;
    uint8_t buffer_count;
    uint8_t closing;
    uint8_t writer_active;
    uint8_t acquire_active;
    uint8_t acquire_new;
    uint8_t mapped_registered;
    uint8_t release_head;
    uint8_t release_count;
    uint8_t release_pending_mask;
    int8_t writing;
    int8_t staged;
    int8_t pending;
    int8_t current;
    int8_t acquired;
    uint32_t acquire_generation;
    uint64_t pending_frame_id;
    uint64_t current_frame_id;
    uint64_t acquired_frame_id;
    uint64_t pending_input_timestamp_us;
    uint64_t current_input_timestamp_us;
    uint64_t acquired_input_timestamp_us;
    uint64_t submitted_frames;
    uint64_t presented_frames;
    uint64_t dropped_frames;
    uint64_t replaced_frames;
    uint64_t released_frames;
    uint64_t last_frame_id;
    pxa_surface_release_t releases[PXA_ESP_SURFACE_MAX_BUFFERS];
    pxa_surface_layer_t layer;
    pxa_surface_damage_rect_t
        opaque_ui_regions[PXA_SURFACE_MAX_OPAQUE_UI_REGIONS];
    uint8_t opaque_ui_region_count;
    uint8_t *raster_textures[PXA_RASTER_MAX_TEXTURES];
    uint16_t raster_texture_width[PXA_RASTER_MAX_TEXTURES];
    uint16_t raster_texture_height[PXA_RASTER_MAX_TEXTURES];
    uint16_t *raster_palette;
    uint16_t raster_palette_light_levels;
    uint16_t *raster_depth_buffer;
    uint8_t *raster_draw_lists[PXA_ESP_RASTER_MAILBOX_SLOTS];
    uint32_t raster_draw_capacities[PXA_ESP_RASTER_MAILBOX_SLOTS];
    pxa_raster_draw_list_view_t
        raster_draw_views[PXA_ESP_RASTER_MAILBOX_SLOTS];
    uint64_t raster_draw_frame_ids[PXA_ESP_RASTER_MAILBOX_SLOTS];
    uint64_t raster_draw_input_timestamps_us[PXA_ESP_RASTER_MAILBOX_SLOTS];
    uint64_t raster_draw_queued_us[PXA_ESP_RASTER_MAILBOX_SLOTS];
    uint64_t raster_buffer_ready_us[PXA_ESP_SURFACE_MAX_BUFFERS];
    int8_t raster_draw_pending;
    int8_t raster_draw_rendering;
    int8_t raster_draw_writing;
    uint64_t raster_last_frame_id;
    pxa_raster_telemetry_t raster_telemetry;
    uint32_t raster_main_us;
    uint32_t raster_worker_us;
    uint16_t raster_split_row;
} pxa_esp_surface_t;

static portMUX_TYPE g_surface_lock = portMUX_INITIALIZER_UNLOCKED;
static pxa_esp_surface_t *g_surface;
static pxa_esp_surface_t *g_acquired_surface;
static pxa_esp_surface_frame_ready_fn g_notify;
static void *g_notify_context;
static pxa_esp_surface_frame_ready_fn g_release_notify;
static void *g_release_notify_context;
static pxa_esp_surface_ui_alpha_provider_fn g_ui_alpha_provider;
static void *g_ui_alpha_provider_context;
static bool g_host_visible = true;
static bool g_composition_required;
static bool g_system_overlay_visible;
static uint32_t g_runtime_modal_count;
static bool g_direct_resume_barrier;
static uint64_t g_direct_resume_after_frame_id;
static uint64_t g_next_input_timestamp_us;
static pxa_esp_surface_input_metrics_t g_input_metrics;
static uint32_t latency_us(uint64_t started_us, uint64_t finished_us);
static bool materialize_latest_raster_draw(void);

#if CONFIG_PXA_PARALLEL_RASTER
typedef struct {
    const uint8_t *bytes;
    pxa_raster_draw_list_view_t list;
    pxa_raster_target_t target;
    pxa_raster_resources_t resources;
    pxa_raster_telemetry_t telemetry;
    uint16_t row_begin;
    uint16_t row_end;
    uint32_t elapsed_us;
} pxa_esp_raster_worker_job_t;

static TaskHandle_t g_raster_worker_task;
static StaticTask_t g_raster_worker_task_storage;
EXT_RAM_BSS_ATTR static StackType_t
    g_raster_worker_stack[CONFIG_PXA_RASTER_WORKER_STACK_SIZE];
static StaticSemaphore_t g_raster_worker_done_storage;
static SemaphoreHandle_t g_raster_worker_done;
static pxa_esp_raster_worker_job_t g_raster_worker_job;
static uint8_t g_raster_worker_attempted;
static uint16_t g_raster_split_row;
static uint16_t g_raster_split_height;

static void raster_worker_task(void *context) {
    (void)context;
    for (;;) {
        uint64_t started_us;
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        started_us = (uint64_t)esp_timer_get_time();
        memset(&g_raster_worker_job.telemetry, 0,
               sizeof(g_raster_worker_job.telemetry));
        pxa_raster_execute_draw_list_rows(
            g_raster_worker_job.bytes, &g_raster_worker_job.list,
            &g_raster_worker_job.target, &g_raster_worker_job.resources,
            g_raster_worker_job.row_begin, g_raster_worker_job.row_end,
            &g_raster_worker_job.telemetry);
        g_raster_worker_job.elapsed_us = latency_us(
            started_us, (uint64_t)esp_timer_get_time());
        xSemaphoreGive(g_raster_worker_done);
    }
}

static void ensure_raster_worker(void) {
    if (g_raster_worker_task != NULL || g_raster_worker_attempted) return;
    g_raster_worker_attempted = 1;
    g_raster_worker_done = xSemaphoreCreateBinaryStatic(
        &g_raster_worker_done_storage);
    if (g_raster_worker_done != NULL) {
        g_raster_worker_task = xTaskCreateStaticPinnedToCore(
            raster_worker_task, "pxa_raster",
            CONFIG_PXA_RASTER_WORKER_STACK_SIZE, NULL,
            CONFIG_PXA_RASTER_WORKER_PRIORITY, g_raster_worker_stack,
            &g_raster_worker_task_storage,
            CONFIG_PXA_RASTER_WORKER_AFFINITY);
    }
    if (g_raster_worker_task == NULL) {
        g_raster_worker_task = NULL;
        ESP_LOGW(PXA_ESP_SURFACE_TAG,
                 "Parallel raster worker unavailable; using one core");
        return;
    }
    ESP_LOGI(PXA_ESP_SURFACE_TAG,
             "Parallel raster worker: core=%d priority=%d stack=%uB PSRAM",
             CONFIG_PXA_RASTER_WORKER_AFFINITY,
             CONFIG_PXA_RASTER_WORKER_PRIORITY,
             (unsigned)CONFIG_PXA_RASTER_WORKER_STACK_SIZE);
}

static uint16_t current_raster_split(uint16_t height) {
    if (g_raster_split_height != height || g_raster_split_row == 0 ||
        g_raster_split_row >= height) {
        g_raster_split_height = height;
        g_raster_split_row = (uint16_t)(((uint32_t)height * 4u) / 5u);
        if (g_raster_split_row == 0) g_raster_split_row = 1;
        if (g_raster_split_row >= height) g_raster_split_row = height - 1u;
    }
    return g_raster_split_row;
}

static void update_raster_split(uint16_t height, uint16_t split,
                                uint32_t main_us, uint32_t worker_us) {
    uint16_t min_split = height / 4u;
    uint16_t max_split = (uint16_t)(((uint32_t)height * 7u) / 8u);
    uint16_t target;
    uint16_t max_step = height / 16u;
    uint64_t main_cost;
    uint64_t worker_cost;

    if (main_us == 0 || worker_us == 0 || split == 0 || split >= height) return;
    if (min_split == 0) min_split = 1;
    if (max_split >= height) max_split = height - 1u;
    if (max_step == 0) max_step = 1;

    /* Balance estimated per-row work without chasing single-frame spikes. */
    main_cost = ((uint64_t)main_us << 10u) / split;
    worker_cost = ((uint64_t)worker_us << 10u) / (height - split);
    target = (uint16_t)(((uint64_t)height * worker_cost) /
                        (main_cost + worker_cost));
    if (target < min_split) target = min_split;
    if (target > max_split) target = max_split;
    if (target > split && target - split > max_step) {
        target = split + max_step;
    } else if (target < split && split - target > max_step) {
        target = split - max_step;
    }
    g_raster_split_row = target;
}
#endif
static uint32_t
    g_input_latency_histograms[3][PXA_ESP_SURFACE_LATENCY_BUCKETS];

enum {
    PXA_ESP_SURFACE_LATENCY_GUEST = 0,
    PXA_ESP_SURFACE_LATENCY_PRESENT,
    PXA_ESP_SURFACE_LATENCY_VISIBLE,
};

static uint32_t latency_us(uint64_t started_us, uint64_t finished_us) {
    const uint64_t elapsed = finished_us >= started_us
                                 ? finished_us - started_us
                                 : 0;
    return elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed;
}

static uint32_t latency_bucket(uint32_t elapsed_us) {
    if (elapsed_us < UINT32_C(32000)) return elapsed_us / 1000u;
    if (elapsed_us < UINT32_C(94000))
        return 32u + (elapsed_us - UINT32_C(32000)) / 2000u;
    return PXA_ESP_SURFACE_LATENCY_BUCKETS - 1u;
}

static uint32_t latency_bucket_upper_us(uint32_t bucket,
                                        uint32_t maximum_us) {
    if (maximum_us == 0) return 0;
    if (bucket < 32u) return (bucket + 1u) * 1000u;
    if (bucket < PXA_ESP_SURFACE_LATENCY_BUCKETS - 1u)
        return UINT32_C(32000) + (bucket - 31u) * 2000u;
    return maximum_us;
}

static void record_latency(uint8_t kind, uint32_t elapsed_us,
                           uint64_t *total_us, uint32_t *maximum_us,
                           uint32_t *count) {
    *total_us += elapsed_us;
    ++*count;
    if (elapsed_us > *maximum_us) *maximum_us = elapsed_us;
    ++g_input_latency_histograms[kind][latency_bucket(elapsed_us)];
}

static uint32_t latency_p95_us(uint8_t kind, uint32_t count,
                               uint32_t maximum_us) {
    const uint32_t target =
        (count / 100u) * 95u + ((count % 100u) * 95u + 99u) / 100u;
    uint32_t seen = 0;
    uint32_t bucket;
    if (count == 0) return 0;
    for (bucket = 0; bucket < PXA_ESP_SURFACE_LATENCY_BUCKETS; ++bucket) {
        seen += g_input_latency_histograms[kind][bucket];
        if (seen >= target)
            return latency_bucket_upper_us(bucket, maximum_us);
    }
    return maximum_us;
}

static void notify_frame_ready(void) {
    pxa_esp_surface_frame_ready_fn callback;
    void *context;
    taskENTER_CRITICAL(&g_surface_lock);
    callback = g_notify;
    context = g_notify_context;
    taskEXIT_CRITICAL(&g_surface_lock);
    if (callback != NULL) callback(context);
}

static void notify_release_ready(void) {
    pxa_esp_surface_frame_ready_fn callback;
    void *context;
    taskENTER_CRITICAL(&g_surface_lock);
    callback = g_release_notify;
    context = g_release_notify_context;
    taskEXIT_CRITICAL(&g_surface_lock);
    if (callback != NULL) callback(context);
}

static void destroy_surface(pxa_esp_surface_t *surface) {
    uint8_t index;
    if (surface == NULL) return;
    surface->magic = 0;
    if ((surface->flags & PXA_SURFACE_FLAG_GUEST_MAPPED) == 0) {
        for (index = 0; index < surface->buffer_count; ++index)
            heap_caps_free(surface->buffers[index]);
    }
    for (index = 0; index < PXA_RASTER_MAX_TEXTURES; ++index)
        heap_caps_free(surface->raster_textures[index]);
    for (index = 0; index < PXA_ESP_RASTER_MAILBOX_SLOTS; ++index)
        heap_caps_free(surface->raster_draw_lists[index]);
    heap_caps_free(surface->raster_palette);
    heap_caps_free(surface->raster_depth_buffer);
    heap_caps_free(surface);
}

static int buffer_is_free(const pxa_esp_surface_t *surface,
                          int index) {
    return index != surface->writing && index != surface->staged &&
           index != surface->pending && index != surface->current &&
           index != surface->acquired &&
           (surface->release_pending_mask & (1u << index)) == 0;
}

static int enqueue_release_locked(pxa_esp_surface_t *surface, int index,
                                  uint64_t frame_id) {
    uint8_t tail;
    if (index < 0 || index >= surface->buffer_count || frame_id == 0 ||
        surface->release_count >= surface->buffer_count ||
        (surface->release_pending_mask & (1u << index)) != 0)
        return 0;
    tail = (uint8_t)((surface->release_head + surface->release_count) %
                     surface->buffer_count);
    memset(&surface->releases[tail], 0, sizeof(surface->releases[tail]));
    surface->releases[tail].buffer_index = (uint8_t)index;
    surface->releases[tail].frame_id = frame_id;
    surface->release_pending_mask |= (uint8_t)(1u << index);
    ++surface->release_count;
    return 1;
}

static uint32_t free_buffer_count(
    const pxa_esp_surface_t *surface) {
    uint32_t count = 0;
    uint8_t index;
    for (index = 0; index < surface->buffer_count; ++index)
        if (buffer_is_free(surface, index)) ++count;
    return count;
}

static uint64_t latest_frame_id_locked(const pxa_esp_surface_t *surface) {
    uint64_t frame_id = surface->last_frame_id;
    if (surface->pending_frame_id > frame_id)
        frame_id = surface->pending_frame_id;
    if (surface->current_frame_id > frame_id)
        frame_id = surface->current_frame_id;
    if (surface->acquired_frame_id > frame_id)
        frame_id = surface->acquired_frame_id;
    if (surface->raster_last_frame_id > frame_id)
        frame_id = surface->raster_last_frame_id;
    return frame_id;
}

static pxa_status_t create_surface(
    void *context, const pxa_surface_desc_t *desc,
    uint64_t *provider_surface, uint32_t *stride_bytes) {
    pxa_esp_surface_t *surface;
    uint32_t frame_bytes;
    uint64_t frame_bytes64;
    uint8_t bytes_per_pixel;
    uint8_t index;
    (void)context;
    if (desc == NULL || provider_surface == NULL || stride_bytes == NULL ||
        desc->width == 0 ||
        desc->height == 0 || desc->buffer_count < 2 ||
        desc->buffer_count > PXA_ESP_SURFACE_MAX_BUFFERS)
        return PXA_STATUS_INVALID_ARGUMENT;
    if ((desc->flags & ~(PXA_SURFACE_FLAG_KNOWN_MASK |
                         PXA_ESP_SURFACE_FLAG_GAME_RENDER)) != 0)
        return PXA_STATUS_UNSUPPORTED;
    if ((desc->flags & PXA_SURFACE_FLAG_GUEST_MAPPED) != 0 &&
        (desc->flags & PXA_ESP_SURFACE_FLAG_GAME_RENDER) != 0)
        return PXA_STATUS_UNSUPPORTED;
    if (desc->format == PXA_SURFACE_FORMAT_RGB565 &&
        (desc->flags & PXA_SURFACE_FLAG_PREMULTIPLIED_ALPHA) == 0)
        bytes_per_pixel = 2;
    else if (desc->format == PXA_SURFACE_FORMAT_ARGB8888_PREMULTIPLIED &&
             desc->flags == PXA_SURFACE_FLAG_PREMULTIPLIED_ALPHA)
        bytes_per_pixel = 4;
    else
        return PXA_STATUS_UNSUPPORTED;
    taskENTER_CRITICAL(&g_surface_lock);
    if (g_surface != NULL) {
        taskEXIT_CRITICAL(&g_surface_lock);
        return PXA_STATUS_RESOURCE_LIMIT;
    }
    taskEXIT_CRITICAL(&g_surface_lock);
    frame_bytes64 = (uint64_t)desc->width * desc->height * bytes_per_pixel;
    if (frame_bytes64 > UINT32_MAX) return PXA_STATUS_RESOURCE_LIMIT;
    frame_bytes = (uint32_t)frame_bytes64;
    surface = heap_caps_calloc(1, sizeof(*surface),
                               MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (surface == NULL) return PXA_STATUS_RESOURCE_LIMIT;
    surface->buffer_count = desc->buffer_count;
    surface->flags = desc->flags;
    if ((desc->flags & PXA_SURFACE_FLAG_GUEST_MAPPED) != 0) {
        if (!PXA_ESP_SURFACE_GUEST_MAPPING_SUPPORTED) {
            ESP_LOGE(PXA_ESP_SURFACE_TAG,
                     "GuestMapped Surface requires "
                     "CONFIG_WAMR_LINEAR_MEMORY_RESERVE_MAX");
            destroy_surface(surface);
            return PXA_STATUS_UNSUPPORTED;
        }
    } else {
        for (index = 0; index < surface->buffer_count; ++index) {
            if ((desc->flags & PXA_ESP_SURFACE_FLAG_GAME_RENDER) != 0) {
                /* Cached PSRAM on purpose. The raster writes every pixel of this
                 * buffer and the DMA-mapped PSRAM pool is several times slower
                 * to write from the CPU (measured ~150 ns per pixel against
                 * ~20 ns for the same store loop in cached memory). Consumers
                 * read the frame either with the CPU (the pai-touch rotation) or
                 * through a DMA that the writer flushes with esp_cache_msync
                 * before handing the frame over. */
                surface->buffers[index] = heap_caps_aligned_calloc(
                    64, 1, frame_bytes,
                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            } else {
                surface->buffers[index] = heap_caps_calloc(
                    1, frame_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            }
            if (surface->buffers[index] == NULL) {
                destroy_surface(surface);
                return PXA_STATUS_RESOURCE_LIMIT;
            }
        }
        if ((desc->flags & PXA_ESP_SURFACE_FLAG_GAME_RENDER) != 0) {
            surface->raster_depth_buffer = heap_caps_aligned_alloc(
                64,
                (size_t)desc->width * desc->height *
                    sizeof(*surface->raster_depth_buffer),
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (surface->raster_depth_buffer == NULL) {
                destroy_surface(surface);
                return PXA_STATUS_RESOURCE_LIMIT;
            }
            for (index = 0; index < PXA_ESP_RASTER_MAILBOX_SLOTS; ++index) {
                surface->raster_draw_lists[index] = heap_caps_malloc(
                    PXA_ESP_RASTER_MAILBOX_MIN_BYTES,
                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (surface->raster_draw_lists[index] == NULL) {
                    destroy_surface(surface);
                    return PXA_STATUS_RESOURCE_LIMIT;
                }
                surface->raster_draw_capacities[index] =
                    PXA_ESP_RASTER_MAILBOX_MIN_BYTES;
            }
        }
    }
    surface->magic = PXA_ESP_SURFACE_MAGIC;
    surface->frame_bytes = frame_bytes;
    surface->stride_bytes = (uint32_t)desc->width * bytes_per_pixel;
    surface->width = desc->width;
    surface->height = desc->height;
    surface->format = desc->format;
    surface->writing = PXA_ESP_SURFACE_NONE;
    surface->staged = PXA_ESP_SURFACE_NONE;
    surface->pending = PXA_ESP_SURFACE_NONE;
    surface->current = PXA_ESP_SURFACE_NONE;
    surface->acquired = PXA_ESP_SURFACE_NONE;
    surface->raster_draw_pending = PXA_ESP_SURFACE_NONE;
    surface->raster_draw_rendering = PXA_ESP_SURFACE_NONE;
    surface->raster_draw_writing = PXA_ESP_SURFACE_NONE;
    surface->layer.width = desc->width;
    surface->layer.height = desc->height;
    taskENTER_CRITICAL(&g_surface_lock);
    if (g_surface != NULL) {
        taskEXIT_CRITICAL(&g_surface_lock);
        destroy_surface(surface);
        return PXA_STATUS_RESOURCE_LIMIT;
    }
    g_surface = surface;
    g_composition_required = false;
    g_direct_resume_barrier = false;
    g_direct_resume_after_frame_id = 0;
    taskEXIT_CRITICAL(&g_surface_lock);
    *provider_surface = (uint64_t)(uintptr_t)surface;
    *stride_bytes = surface->stride_bytes;
    ESP_LOGI(PXA_ESP_SURFACE_TAG,
             "Created: %ux%u format=%u buffers=%u flags=0x%02x mapped=%u",
             (unsigned)surface->width, (unsigned)surface->height,
             (unsigned)surface->format, (unsigned)surface->buffer_count,
             (unsigned)surface->flags,
             (unsigned)((surface->flags & PXA_SURFACE_FLAG_GUEST_MAPPED) != 0));
    return PXA_STATUS_OK;
}

static pxa_status_t register_surface_buffers(
    void *context, uint64_t provider_surface, uint8_t *pixels, size_t size) {
    pxa_esp_surface_t *surface =
        (pxa_esp_surface_t *)(uintptr_t)provider_surface;
    uintptr_t native_alignment_mask;
    uint8_t index;
    (void)context;
    if (surface == NULL || pixels == NULL) return PXA_STATUS_INVALID_ARGUMENT;
    taskENTER_CRITICAL(&g_surface_lock);
    if (surface->magic != PXA_ESP_SURFACE_MAGIC || surface->closing ||
        (surface->flags & PXA_SURFACE_FLAG_GUEST_MAPPED) == 0) {
        taskEXIT_CRITICAL(&g_surface_lock);
        return PXA_STATUS_BAD_STATE;
    }
    /* The Guest SDK enforces the 64-byte Wasm offset contract. Translation
     * adds the runtime's pinned linear-memory base, which is not required to
     * retain that alignment. These pixels are CPU-read into a separate DMA
     * output buffer, so the translated address only needs format alignment. */
    native_alignment_mask =
        surface->format == PXA_SURFACE_FORMAT_ARGB8888_PREMULTIPLIED ? 3u : 1u;
    if (((uintptr_t)pixels & native_alignment_mask) != 0) {
        taskEXIT_CRITICAL(&g_surface_lock);
        return PXA_STATUS_INVALID_ARGUMENT;
    }
    if (surface->mapped_registered) {
        taskEXIT_CRITICAL(&g_surface_lock);
        return PXA_STATUS_BAD_STATE;
    }
    if (size != (size_t)surface->frame_bytes * surface->buffer_count) {
        taskEXIT_CRITICAL(&g_surface_lock);
        return PXA_STATUS_INVALID_ARGUMENT;
    }
    for (index = 0; index < surface->buffer_count; ++index)
        surface->buffers[index] = pixels + (size_t)index * surface->frame_bytes;
    surface->mapped_registered = 1;
    taskEXIT_CRITICAL(&g_surface_lock);
    ESP_LOGI(PXA_ESP_SURFACE_TAG,
             "Guest buffers registered: frame_bytes=%u count=%u total=%u",
             (unsigned)surface->frame_bytes, (unsigned)surface->buffer_count,
             (unsigned)size);
    return PXA_STATUS_OK;
}

static pxa_status_t acquire_surface_buffer(
    void *context, uint64_t provider_surface, uint8_t *buffer_index) {
    pxa_esp_surface_t *surface =
        (pxa_esp_surface_t *)(uintptr_t)provider_surface;
    uint8_t index;
    (void)context;
    if (surface == NULL || buffer_index == NULL)
        return PXA_STATUS_INVALID_ARGUMENT;
    taskENTER_CRITICAL(&g_surface_lock);
    if (surface->magic != PXA_ESP_SURFACE_MAGIC || surface->closing ||
        !surface->mapped_registered) {
        taskEXIT_CRITICAL(&g_surface_lock);
        return PXA_STATUS_BAD_STATE;
    }
    if (surface->writing != PXA_ESP_SURFACE_NONE) {
        taskEXIT_CRITICAL(&g_surface_lock);
        return PXA_STATUS_BAD_STATE;
    }
    for (index = 0; index < surface->buffer_count; ++index) {
        if (buffer_is_free(surface, index)) {
            surface->writing = (int8_t)index;
            *buffer_index = index;
            taskEXIT_CRITICAL(&g_surface_lock);
            return PXA_STATUS_OK;
        }
    }
    taskEXIT_CRITICAL(&g_surface_lock);
    return PXA_STATUS_WOULD_BLOCK;
}

static pxa_status_t present_surface_buffer(
    void *context, uint64_t provider_surface, uint8_t buffer_index,
    uint64_t frame_id) {
    pxa_esp_surface_t *surface =
        (pxa_esp_surface_t *)(uintptr_t)provider_surface;
    (void)context;
    if (surface == NULL || frame_id == 0)
        return PXA_STATUS_INVALID_ARGUMENT;
    const uint64_t present_call_us = (uint64_t)esp_timer_get_time();
    uint64_t input_timestamp_us;
    int release_ready = 0;
    int first_frame = 0;
    taskENTER_CRITICAL(&g_surface_lock);
    if (surface->magic != PXA_ESP_SURFACE_MAGIC || surface->closing ||
        !surface->mapped_registered) {
        taskEXIT_CRITICAL(&g_surface_lock);
        return PXA_STATUS_BAD_STATE;
    }
    if (buffer_index >= surface->buffer_count ||
        surface->writing != (int8_t)buffer_index ||
        frame_id <= surface->last_frame_id) {
        taskEXIT_CRITICAL(&g_surface_lock);
        return PXA_STATUS_BAD_STATE;
    }
    if (surface->pending != PXA_ESP_SURFACE_NONE &&
        !enqueue_release_locked(surface, surface->pending,
                                surface->pending_frame_id)) {
        taskEXIT_CRITICAL(&g_surface_lock);
        return PXA_STATUS_WOULD_BLOCK;
    }
    if (surface->pending != PXA_ESP_SURFACE_NONE) {
        release_ready = 1;
        ++surface->dropped_frames;
        ++surface->replaced_frames;
    }
    input_timestamp_us = g_next_input_timestamp_us != 0
                             ? g_next_input_timestamp_us
                             : surface->pending_input_timestamp_us;
    if (g_next_input_timestamp_us != 0) {
        const uint32_t elapsed = latency_us(g_next_input_timestamp_us,
                                            present_call_us);
        record_latency(PXA_ESP_SURFACE_LATENCY_PRESENT, elapsed,
                       &g_input_metrics.sample_to_present_total_us,
                       &g_input_metrics.sample_to_present_max_us,
                       &g_input_metrics.sample_to_present_count);
    }
    surface->pending = (int8_t)buffer_index;
    surface->pending_frame_id = frame_id;
    surface->pending_input_timestamp_us = input_timestamp_us;
    g_next_input_timestamp_us = 0;
    surface->last_frame_id = frame_id;
    surface->writing = PXA_ESP_SURFACE_NONE;
    first_frame = surface->submitted_frames == 0;
    ++surface->submitted_frames;
    taskEXIT_CRITICAL(&g_surface_lock);
    if (first_frame) {
        ESP_LOGI(PXA_ESP_SURFACE_TAG,
                 "First frame submitted: id=%llu buffer=%u",
                 (unsigned long long)frame_id, (unsigned)buffer_index);
    }
    if (release_ready) notify_release_ready();
    notify_frame_ready();
    return PXA_STATUS_OK;
}

static pxa_status_t peek_surface_release(
    void *context, uint64_t provider_surface, pxa_surface_release_t *release) {
    pxa_esp_surface_t *surface =
        (pxa_esp_surface_t *)(uintptr_t)provider_surface;
    (void)context;
    if (surface == NULL || release == NULL) return PXA_STATUS_INVALID_ARGUMENT;
    taskENTER_CRITICAL(&g_surface_lock);
    if (surface->magic != PXA_ESP_SURFACE_MAGIC || surface->closing) {
        taskEXIT_CRITICAL(&g_surface_lock);
        return PXA_STATUS_BAD_STATE;
    }
    if (surface->release_count == 0) {
        taskEXIT_CRITICAL(&g_surface_lock);
        return PXA_STATUS_WOULD_BLOCK;
    }
    *release = surface->releases[surface->release_head];
    taskEXIT_CRITICAL(&g_surface_lock);
    return PXA_STATUS_OK;
}

static void consume_surface_release(void *context, uint64_t provider_surface) {
    pxa_esp_surface_t *surface =
        (pxa_esp_surface_t *)(uintptr_t)provider_surface;
    pxa_surface_release_t *release;
    (void)context;
    if (surface == NULL) return;
    taskENTER_CRITICAL(&g_surface_lock);
    if (surface->magic != PXA_ESP_SURFACE_MAGIC || surface->closing ||
        surface->release_count == 0) {
        taskEXIT_CRITICAL(&g_surface_lock);
        return;
    }
    release = &surface->releases[surface->release_head];
    surface->release_pending_mask &=
        (uint8_t)~(uint8_t)(1u << release->buffer_index);
    memset(release, 0, sizeof(*release));
    surface->release_head =
        (uint8_t)((surface->release_head + 1u) % surface->buffer_count);
    --surface->release_count;
    ++surface->released_frames;
    taskEXIT_CRITICAL(&g_surface_lock);
}

static pxa_status_t write_surface(void *context, uint64_t provider_surface,
                                  const uint8_t *pixels, size_t size) {
    pxa_esp_surface_t *surface =
        (pxa_esp_surface_t *)(uintptr_t)provider_surface;
    int index = PXA_ESP_SURFACE_NONE;
    uint8_t cursor;
    int destroy = 0;
    (void)context;
    if (surface == NULL || pixels == NULL) return PXA_STATUS_INVALID_ARGUMENT;
    taskENTER_CRITICAL(&g_surface_lock);
    if (surface->magic != PXA_ESP_SURFACE_MAGIC ||
        surface->closing || size != surface->frame_bytes ||
        (surface->flags & (PXA_SURFACE_FLAG_GUEST_MAPPED |
                           PXA_ESP_SURFACE_FLAG_GAME_RENDER)) != 0) {
        taskEXIT_CRITICAL(&g_surface_lock);
        return PXA_STATUS_BAD_STATE;
    }
    if (surface->staged != PXA_ESP_SURFACE_NONE ||
        surface->writer_active) {
        taskEXIT_CRITICAL(&g_surface_lock);
        return PXA_STATUS_WOULD_BLOCK;
    }
    for (cursor = 0; cursor < surface->buffer_count; ++cursor) {
        if (buffer_is_free(surface, cursor)) {
            index = cursor;
            break;
        }
    }
    if (index == PXA_ESP_SURFACE_NONE) {
        taskEXIT_CRITICAL(&g_surface_lock);
        return PXA_STATUS_WOULD_BLOCK;
    }
    surface->writing = (int8_t)index;
    surface->writer_active = 1;
    taskEXIT_CRITICAL(&g_surface_lock);

    memcpy(surface->buffers[index], pixels, size);

    taskENTER_CRITICAL(&g_surface_lock);
    surface->writer_active = 0;
    surface->writing = PXA_ESP_SURFACE_NONE;
    if (!surface->closing)
        surface->staged = (int8_t)index;
    else if (!surface->acquire_active)
        destroy = 1;
    taskEXIT_CRITICAL(&g_surface_lock);
    if (destroy) destroy_surface(surface);
    return destroy ? PXA_STATUS_BAD_STATE : PXA_STATUS_OK;
}

static pxa_status_t queue_surface(
    void *context, uint64_t provider_surface, uint64_t frame_id,
    const pxa_surface_damage_rect_t *damage, uint8_t damage_count) {
    pxa_esp_surface_t *surface =
        (pxa_esp_surface_t *)(uintptr_t)provider_surface;
    (void)context;
    (void)damage;
    (void)damage_count;
    if (surface == NULL || frame_id == 0) return PXA_STATUS_INVALID_ARGUMENT;
    taskENTER_CRITICAL(&g_surface_lock);
    if (surface->magic != PXA_ESP_SURFACE_MAGIC || surface->closing ||
        (surface->flags & (PXA_SURFACE_FLAG_GUEST_MAPPED |
                           PXA_ESP_SURFACE_FLAG_GAME_RENDER)) != 0) {
        taskEXIT_CRITICAL(&g_surface_lock);
        return PXA_STATUS_BAD_STATE;
    }
    if (surface->staged == PXA_ESP_SURFACE_NONE) {
        taskEXIT_CRITICAL(&g_surface_lock);
        return PXA_STATUS_BAD_STATE;
    }
    if (surface->pending != PXA_ESP_SURFACE_NONE)
        ++surface->dropped_frames;
    surface->pending = surface->staged;
    surface->staged = PXA_ESP_SURFACE_NONE;
    surface->pending_frame_id = frame_id;
    ++surface->submitted_frames;
    taskEXIT_CRITICAL(&g_surface_lock);
    notify_frame_ready();
    return PXA_STATUS_OK;
}

static uint32_t raster_capabilities(void) {
    return PXA_RASTER_CAP_FLAT_QUAD | PXA_RASTER_CAP_TEXTURED_QUAD |
           PXA_RASTER_CAP_ADDITIVE_SPRITE | PXA_RASTER_CAP_SPRITE_BATCH |
           PXA_RASTER_CAP_TRIANGLE_BATCH | PXA_RASTER_CAP_AFFINE_UV |
           PXA_RASTER_CAP_TEXTURE_SLOTS_48 |
           PXA_RASTER_CAP_PAINTER_POLYGON |
           PXA_RASTER_CAP_LIT_PALETTE_DEPTH |
           PXA_RASTER_CAP_DEPTH_CUTOUT |
           PXA_RASTER_CAP_FIXED_ALPHA_BLEND;
}

static uint8_t *allocate_raster_resource(size_t size) {
    uint8_t *memory = heap_caps_malloc(
        size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (memory != NULL) return memory;
    memory = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (memory != NULL)
        ESP_LOGW(PXA_ESP_SURFACE_TAG,
                 "%u B raster resource placed in PSRAM",
                 (unsigned)size);
    return memory;
}

static void copy_raster_resources(const pxa_esp_surface_t *surface,
                                  pxa_raster_resources_t *resources) {
    uint8_t index;
    memset(resources, 0, sizeof(*resources));
    resources->palette = surface->raster_palette;
    resources->palette_light_levels = surface->raster_palette_light_levels;
    resources->capabilities = raster_capabilities();
    for (index = 0; index < PXA_RASTER_MAX_TEXTURES; ++index) {
        resources->textures[index].pixels = surface->raster_textures[index];
        resources->textures[index].width = surface->raster_texture_width[index];
        resources->textures[index].height = surface->raster_texture_height[index];
    }
}

static pxa_status_t raster_upload_surface(void *context,
                                          uint64_t provider_surface,
                                          const uint8_t *bytes, size_t size) {
    pxa_esp_surface_t *surface =
        (pxa_esp_surface_t *)(uintptr_t)provider_surface;
    pxa_raster_upload_view_t upload;
    uint8_t *replacement;
    uint8_t *previous;
    pxa_status_t status;
    (void)context;
    if (surface == NULL || bytes == NULL) return PXA_STATUS_INVALID_ARGUMENT;
    status = pxa_raster_decode_upload(bytes, size, &upload);
    if (status != PXA_STATUS_OK) return status;
    replacement = allocate_raster_resource(upload.payload_bytes);
    if (replacement == NULL) return PXA_STATUS_RESOURCE_LIMIT;
    if (upload.kind == PXA_RASTER_UPLOAD_PALETTE_RGB565 ||
        upload.kind == PXA_RASTER_UPLOAD_LIT_PALETTE_RGB565) {
        uint32_t index;
        const uint32_t entries = upload.payload_bytes / sizeof(uint16_t);
        for (index = 0; index < entries; ++index)
            ((uint16_t *)replacement)[index] =
                pxa_read_u16(upload.payload + (size_t)index * 2u);
    } else {
        memcpy(replacement, upload.payload, upload.payload_bytes);
    }
    taskENTER_CRITICAL(&g_surface_lock);
    if (surface->magic != PXA_ESP_SURFACE_MAGIC || surface->closing ||
        (surface->flags & PXA_ESP_SURFACE_FLAG_GAME_RENDER) == 0 ||
        surface->raster_last_frame_id != 0 ||
        surface->raster_draw_pending != PXA_ESP_SURFACE_NONE ||
        surface->raster_draw_rendering != PXA_ESP_SURFACE_NONE ||
        surface->raster_draw_writing != PXA_ESP_SURFACE_NONE) {
        taskEXIT_CRITICAL(&g_surface_lock);
        heap_caps_free(replacement);
        return PXA_STATUS_BAD_STATE;
    }
    if (upload.kind == PXA_RASTER_UPLOAD_PALETTE_RGB565 ||
        upload.kind == PXA_RASTER_UPLOAD_LIT_PALETTE_RGB565) {
        previous = (uint8_t *)surface->raster_palette;
        surface->raster_palette = (uint16_t *)replacement;
        surface->raster_palette_light_levels = upload.height;
    } else {
        previous = surface->raster_textures[upload.slot];
        surface->raster_textures[upload.slot] = replacement;
        surface->raster_texture_width[upload.slot] = upload.width;
        surface->raster_texture_height[upload.slot] = upload.height;
    }
    taskEXIT_CRITICAL(&g_surface_lock);
    heap_caps_free(previous);
    return PXA_STATUS_OK;
}

static pxa_status_t raster_submit_surface(void *context,
                                          uint64_t provider_surface,
                                          const uint8_t *bytes, size_t size) {
    pxa_esp_surface_t *surface =
        (pxa_esp_surface_t *)(uintptr_t)provider_surface;
    pxa_raster_resources_t resources;
    pxa_raster_target_t target;
    pxa_raster_draw_list_view_t list;
    pxa_status_t status;
    uint8_t *mailbox;
    uint8_t *replacement = NULL;
    uint8_t *previous = NULL;
    uint32_t capacity;
    uint32_t replacement_capacity = 0;
    uint64_t submit_us;
    uint64_t input_timestamp_us;
    int replaced_pending;
    int mailbox_index;
    int mailbox_overwritten = 0;
    int submit_busy;
    int destroy = 0;
    (void)context;
    if (surface == NULL || bytes == NULL) return PXA_STATUS_INVALID_ARGUMENT;
    if (size < PXA_RASTER_DRAW_HEADER_BYTES ||
        size > PXA_RASTER_MAX_DRAW_BYTES)
        return PXA_STATUS_LIMIT_EXCEEDED;
    submit_us = (uint64_t)esp_timer_get_time();
    taskENTER_CRITICAL(&g_surface_lock);
    if (surface->magic != PXA_ESP_SURFACE_MAGIC || surface->closing ||
        (surface->flags & PXA_ESP_SURFACE_FLAG_GAME_RENDER) == 0 ||
        surface->raster_palette == NULL ||
        surface->raster_draw_writing != PXA_ESP_SURFACE_NONE) {
        submit_busy = surface->raster_draw_writing != PXA_ESP_SURFACE_NONE;
        taskEXIT_CRITICAL(&g_surface_lock);
        return submit_busy ? PXA_STATUS_WOULD_BLOCK : PXA_STATUS_BAD_STATE;
    }
    copy_raster_resources(surface, &resources);
    target.pixels = (uint16_t *)surface->buffers[0];
    target.depth_pixels = surface->raster_depth_buffer;
    target.stride_pixels = surface->stride_bytes / sizeof(uint16_t);
    target.depth_stride_pixels = surface->width;
    target.width = surface->width;
    target.height = surface->height;
    replaced_pending = surface->raster_draw_pending;
    mailbox_index = replaced_pending;
    if (mailbox_index == PXA_ESP_SURFACE_NONE)
        mailbox_index = surface->raster_draw_rendering == 0 ? 1 : 0;
    input_timestamp_us =
        g_next_input_timestamp_us != 0
            ? g_next_input_timestamp_us
            : (replaced_pending != PXA_ESP_SURFACE_NONE
                   ? surface->raster_draw_input_timestamps_us[mailbox_index]
                   : 0);
    surface->raster_draw_pending = PXA_ESP_SURFACE_NONE;
    surface->raster_draw_writing = (int8_t)mailbox_index;
    mailbox = surface->raster_draw_lists[mailbox_index];
    capacity = surface->raster_draw_capacities[mailbox_index];
    ++surface->writer_active;
    taskEXIT_CRITICAL(&g_surface_lock);

    if (size > capacity) {
        replacement_capacity =
            ((uint32_t)size + PXA_ESP_RASTER_MAILBOX_GROW_BYTES - 1u) /
            PXA_ESP_RASTER_MAILBOX_GROW_BYTES *
            PXA_ESP_RASTER_MAILBOX_GROW_BYTES;
        if (replacement_capacity > PXA_RASTER_MAX_DRAW_BYTES)
            replacement_capacity = PXA_RASTER_MAX_DRAW_BYTES;
        replacement = heap_caps_malloc(
            replacement_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (replacement == NULL) {
            status = PXA_STATUS_RESOURCE_LIMIT;
        } else {
            mailbox = replacement;
            memcpy(mailbox, bytes, size);
            status = pxa_raster_validate_draw_list(
                mailbox, size, &target, &resources, &list);
        }
    } else {
        memcpy(mailbox, bytes, size);
        mailbox_overwritten = 1;
        status = pxa_raster_validate_draw_list(mailbox, size, &target,
                                               &resources, &list);
    }

    taskENTER_CRITICAL(&g_surface_lock);
    --surface->writer_active;
    surface->raster_draw_writing = PXA_ESP_SURFACE_NONE;
    if (surface->magic != PXA_ESP_SURFACE_MAGIC || surface->closing) {
        destroy = surface->magic == PXA_ESP_SURFACE_MAGIC &&
                  !surface->writer_active && !surface->acquire_active;
        taskEXIT_CRITICAL(&g_surface_lock);
        heap_caps_free(replacement);
        if (destroy) destroy_surface(surface);
        return PXA_STATUS_CANCELLED;
    }
    if (status != PXA_STATUS_OK ||
        list.frame_id <= surface->raster_last_frame_id) {
        if (replaced_pending != PXA_ESP_SURFACE_NONE && mailbox_overwritten) {
            ++surface->dropped_frames;
            ++surface->replaced_frames;
            ++surface->raster_telemetry.dropped_frames;
        } else if (replaced_pending != PXA_ESP_SURFACE_NONE) {
            surface->raster_draw_pending = (int8_t)mailbox_index;
        }
        ++surface->raster_telemetry.rejected_lists;
        taskEXIT_CRITICAL(&g_surface_lock);
        heap_caps_free(replacement);
        if (replaced_pending != PXA_ESP_SURFACE_NONE && !mailbox_overwritten)
            notify_frame_ready();
        return status != PXA_STATUS_OK ? status : PXA_STATUS_BAD_STATE;
    }
    if (replacement != NULL) {
        previous = surface->raster_draw_lists[mailbox_index];
        surface->raster_draw_lists[mailbox_index] = replacement;
        surface->raster_draw_capacities[mailbox_index] = replacement_capacity;
        replacement = NULL;
    }
    if (replaced_pending != PXA_ESP_SURFACE_NONE) {
        ++surface->dropped_frames;
        ++surface->replaced_frames;
        ++surface->raster_telemetry.dropped_frames;
    }
    surface->raster_draw_views[mailbox_index] = list;
    surface->raster_draw_frame_ids[mailbox_index] = list.frame_id;
    surface->raster_draw_input_timestamps_us[mailbox_index] =
        input_timestamp_us;
    surface->raster_draw_queued_us[mailbox_index] = submit_us;
    surface->raster_draw_pending = (int8_t)mailbox_index;
    surface->last_frame_id = list.frame_id;
    surface->raster_last_frame_id = list.frame_id;
    if (g_next_input_timestamp_us != 0) {
        const uint32_t elapsed = latency_us(g_next_input_timestamp_us,
                                            submit_us);
        record_latency(PXA_ESP_SURFACE_LATENCY_PRESENT, elapsed,
                       &g_input_metrics.sample_to_present_total_us,
                       &g_input_metrics.sample_to_present_max_us,
                       &g_input_metrics.sample_to_present_count);
    }
    g_next_input_timestamp_us = 0;
    ++surface->submitted_frames;
    ++surface->raster_telemetry.submitted_frames;
    taskEXIT_CRITICAL(&g_surface_lock);
    heap_caps_free(previous);
    /* Keep GameRender's execution model aligned with the original HostSurface:
     * rasterize on the submitting runtime task, then hand the completed buffer
     * to the independent presenter. This lets the next frame's CPU raster work
     * overlap the previous frame's PPA copy instead of serializing both stages
     * in the presenter task. acquire_latest() retains its materialize call as a
     * fallback when no output buffer was free at submission time. */
    (void)materialize_latest_raster_draw();
    notify_frame_ready();
    return PXA_STATUS_OK;
}

static pxa_status_t raster_query_surface(void *context,
                                         uint64_t provider_surface,
                                         pxa_raster_telemetry_t *telemetry) {
    pxa_esp_surface_t *surface =
        (pxa_esp_surface_t *)(uintptr_t)provider_surface;
    (void)context;
    if (surface == NULL || telemetry == NULL)
        return PXA_STATUS_INVALID_ARGUMENT;
    taskENTER_CRITICAL(&g_surface_lock);
    if (surface->magic != PXA_ESP_SURFACE_MAGIC || surface->closing ||
        (surface->flags & PXA_ESP_SURFACE_FLAG_GAME_RENDER) == 0) {
        taskEXIT_CRITICAL(&g_surface_lock);
        return PXA_STATUS_BAD_STATE;
    }
    *telemetry = surface->raster_telemetry;
    taskEXIT_CRITICAL(&g_surface_lock);
    return PXA_STATUS_OK;
}

static pxa_status_t configure_surface(
    void *context, uint64_t provider_surface,
    const pxa_surface_layer_t *layer) {
    pxa_esp_surface_t *surface =
        (pxa_esp_surface_t *)(uintptr_t)provider_surface;
    (void)context;
    if (surface == NULL || layer == NULL) return PXA_STATUS_INVALID_ARGUMENT;
    taskENTER_CRITICAL(&g_surface_lock);
    if (surface->magic != PXA_ESP_SURFACE_MAGIC || surface->closing) {
        taskEXIT_CRITICAL(&g_surface_lock);
        return PXA_STATUS_BAD_STATE;
    }
    surface->layer = *layer;
    taskEXIT_CRITICAL(&g_surface_lock);
    notify_frame_ready();
    return PXA_STATUS_OK;
}

static pxa_status_t configure_opaque_ui_regions_surface(
    void *context, uint64_t provider_surface,
    const pxa_surface_damage_rect_t *regions, uint8_t region_count) {
    pxa_esp_surface_t *surface =
        (pxa_esp_surface_t *)(uintptr_t)provider_surface;
    (void)context;
    if (surface == NULL ||
        (region_count != 0 && regions == NULL) ||
        region_count > PXA_SURFACE_MAX_OPAQUE_UI_REGIONS)
        return PXA_STATUS_INVALID_ARGUMENT;
    taskENTER_CRITICAL(&g_surface_lock);
    if (surface->magic != PXA_ESP_SURFACE_MAGIC || surface->closing) {
        taskEXIT_CRITICAL(&g_surface_lock);
        return PXA_STATUS_BAD_STATE;
    }
    if (region_count != 0)
        memcpy(surface->opaque_ui_regions, regions,
               (size_t)region_count * sizeof(regions[0]));
    surface->opaque_ui_region_count = region_count;
    taskEXIT_CRITICAL(&g_surface_lock);
    notify_frame_ready();
    return PXA_STATUS_OK;
}

static pxa_status_t query_surface(void *context, uint64_t provider_surface,
                                  pxa_surface_state_t *state) {
    pxa_esp_surface_t *surface =
        (pxa_esp_surface_t *)(uintptr_t)provider_surface;
    pxa_esp_surface_ui_alpha_provider_fn ui_alpha_provider;
    void *ui_alpha_provider_context;
    pxa_esp_surface_ui_alpha_plane_t ui_alpha_plane;
    (void)context;
    if (surface == NULL || state == NULL) return PXA_STATUS_INVALID_ARGUMENT;
    taskENTER_CRITICAL(&g_surface_lock);
    if (surface->magic != PXA_ESP_SURFACE_MAGIC || surface->closing) {
        taskEXIT_CRITICAL(&g_surface_lock);
        return PXA_STATUS_BAD_STATE;
    }
    state->submitted_frames = surface->submitted_frames;
    state->presented_frames = surface->presented_frames;
    state->dropped_frames = surface->dropped_frames;
    state->replaced_frames = surface->replaced_frames;
    state->released_frames = surface->released_frames;
    state->free_buffers = free_buffer_count(surface);
    state->flags = PXA_SURFACE_STATE_FLAG_SUPPORTS_OPAQUE_UI_REGIONS |
                   PXA_SURFACE_STATE_FLAG_SUPPORTS_ALPHA_COMPOSITING;
    if (PXA_ESP_SURFACE_GUEST_MAPPING_SUPPORTED)
        state->flags |= PXA_SURFACE_STATE_FLAG_SUPPORTS_GUEST_MAPPED;
    ui_alpha_provider = g_ui_alpha_provider;
    ui_alpha_provider_context = g_ui_alpha_provider_context;
    taskEXIT_CRITICAL(&g_surface_lock);
    memset(&ui_alpha_plane, 0, sizeof(ui_alpha_plane));
    if (ui_alpha_provider != NULL &&
        ui_alpha_provider(ui_alpha_provider_context, &ui_alpha_plane) &&
        ui_alpha_plane.visible && ui_alpha_plane.pixels != NULL &&
        ui_alpha_plane.alpha != NULL && ui_alpha_plane.width != 0 &&
        ui_alpha_plane.height != 0) {
        state->flags |= PXA_SURFACE_STATE_FLAG_UI_ALPHA_PLANE_ACTIVE;
    }
    return PXA_STATUS_OK;
}

static void close_surface(void *context, uint64_t provider_surface) {
    pxa_esp_surface_t *surface =
        (pxa_esp_surface_t *)(uintptr_t)provider_surface;
    int destroy;
    (void)context;
    if (surface == NULL) return;
    taskENTER_CRITICAL(&g_surface_lock);
    if (surface->magic != PXA_ESP_SURFACE_MAGIC) {
        taskEXIT_CRITICAL(&g_surface_lock);
        return;
    }
    surface->closing = 1;
    surface->layer.visible = 0;
    if (g_surface == surface) {
        g_surface = NULL;
        g_next_input_timestamp_us = 0;
        g_direct_resume_barrier = false;
        g_direct_resume_after_frame_id = 0;
    }
    destroy = !surface->writer_active && !surface->acquire_active;
    taskEXIT_CRITICAL(&g_surface_lock);
    notify_frame_ready();
    if (destroy) destroy_surface(surface);
}

void pxa_esp_surface_backend(pxa_surface_backend_t *backend) {
    if (backend == NULL) return;
    memset(backend, 0, sizeof(*backend));
    backend->struct_size = sizeof(*backend);
    backend->create = create_surface;
    backend->write = write_surface;
    backend->queue = queue_surface;
    backend->configure = configure_surface;
    backend->configure_opaque_ui_regions = configure_opaque_ui_regions_surface;
    backend->query = query_surface;
    backend->close = close_surface;
    backend->register_buffers = register_surface_buffers;
    backend->acquire_buffer = acquire_surface_buffer;
    backend->present_buffer = present_surface_buffer;
    backend->peek_release = peek_surface_release;
    backend->consume_release = consume_surface_release;
}

static pxa_status_t create_game_render_context(
    void *context, const pxa_game_render_desc_t *desc,
    uint64_t *provider_context, uint32_t *capabilities) {
    pxa_surface_desc_t surface_desc;
    pxa_surface_layer_t layer;
    uint32_t stride;
    pxa_status_t status;
    if (desc == NULL || provider_context == NULL || capabilities == NULL)
        return PXA_STATUS_INVALID_ARGUMENT;
    memset(&surface_desc, 0, sizeof(surface_desc));
    surface_desc.width = desc->width;
    surface_desc.height = desc->height;
    surface_desc.format = PXA_SURFACE_FORMAT_RGB565;
    surface_desc.buffer_count = desc->buffer_count;
    surface_desc.flags = PXA_ESP_SURFACE_FLAG_GAME_RENDER;
    if ((desc->flags & PXA_GAME_RENDER_FLAG_PREFER_DIRECT_SCANOUT) != 0)
        surface_desc.flags |= PXA_SURFACE_FLAG_PREFER_DIRECT_SCANOUT;
    status = create_surface(context, &surface_desc, provider_context, &stride);
    if (status != PXA_STATUS_OK) return status;
    memset(&layer, 0, sizeof(layer));
    layer.width = desc->width;
    layer.height = desc->height;
    layer.visible = 1;
    status = configure_surface(context, *provider_context, &layer);
    if (status != PXA_STATUS_OK) {
        close_surface(context, *provider_context);
        *provider_context = 0;
        return status;
    }
#if CONFIG_PXA_PARALLEL_RASTER
    ensure_raster_worker();
#endif
    *capabilities = raster_capabilities();
    return PXA_STATUS_OK;
}

void pxa_esp_game_render_backend(pxa_game_render_backend_t *backend) {
    if (backend == NULL) return;
    memset(backend, 0, sizeof(*backend));
    backend->struct_size = sizeof(*backend);
    backend->create = create_game_render_context;
    backend->upload = raster_upload_surface;
    backend->submit = raster_submit_surface;
    backend->query = raster_query_surface;
    backend->close = close_surface;
}

void pxa_esp_surface_set_frame_ready_callback(
    pxa_esp_surface_frame_ready_fn callback, void *context) {
    taskENTER_CRITICAL(&g_surface_lock);
    g_notify = callback;
    g_notify_context = context;
    taskEXIT_CRITICAL(&g_surface_lock);
}

void pxa_esp_surface_set_release_ready_callback(
    pxa_esp_surface_frame_ready_fn callback, void *context) {
    taskENTER_CRITICAL(&g_surface_lock);
    g_release_notify = callback;
    g_release_notify_context = context;
    taskEXIT_CRITICAL(&g_surface_lock);
}

void pxa_esp_surface_set_ui_alpha_provider(
    pxa_esp_surface_ui_alpha_provider_fn callback, void *context) {
    taskENTER_CRITICAL(&g_surface_lock);
    g_ui_alpha_provider = callback;
    g_ui_alpha_provider_context = context;
    taskEXIT_CRITICAL(&g_surface_lock);
    notify_frame_ready();
}

void pxa_esp_surface_require_composition(void) {
    taskENTER_CRITICAL(&g_surface_lock);
    g_composition_required = true;
    taskEXIT_CRITICAL(&g_surface_lock);
    notify_frame_ready();
}

bool pxa_esp_surface_composition_required(void) {
    bool required;
    bool ui_required;
    pxa_esp_surface_ui_alpha_provider_fn ui_alpha_provider;
    void *ui_alpha_provider_context;
    pxa_esp_surface_ui_alpha_plane_t ui_alpha_plane;
    taskENTER_CRITICAL(&g_surface_lock);
    required = g_system_overlay_visible || g_runtime_modal_count != 0;
    ui_required = g_composition_required;
    ui_alpha_provider = g_ui_alpha_provider;
    ui_alpha_provider_context = g_ui_alpha_provider_context;
    taskEXIT_CRITICAL(&g_surface_lock);
    if (required || !ui_required) return required;
    if (ui_alpha_provider == NULL) return true;
    memset(&ui_alpha_plane, 0, sizeof(ui_alpha_plane));
    if (!ui_alpha_provider(ui_alpha_provider_context, &ui_alpha_plane))
        return true;
    required = ui_alpha_plane.visible && ui_alpha_plane.pixels != NULL &&
               ui_alpha_plane.alpha != NULL && ui_alpha_plane.width != 0 &&
               ui_alpha_plane.height != 0;
    return required;
}

void pxa_esp_surface_set_system_overlay_visible(bool visible) {
    bool changed;
    taskENTER_CRITICAL(&g_surface_lock);
    changed = g_system_overlay_visible != visible;
    g_system_overlay_visible = visible;
    if (changed && !visible && g_surface != NULL) {
        g_direct_resume_barrier = true;
        g_direct_resume_after_frame_id = latest_frame_id_locked(g_surface);
    }
    taskEXIT_CRITICAL(&g_surface_lock);
    if (changed) notify_frame_ready();
}

void pxa_esp_surface_runtime_modal_enter(void) {
    bool changed = false;
    taskENTER_CRITICAL(&g_surface_lock);
    if (g_runtime_modal_count != UINT32_MAX) {
        changed = g_runtime_modal_count == 0;
        ++g_runtime_modal_count;
    }
    taskEXIT_CRITICAL(&g_surface_lock);
    if (changed) notify_frame_ready();
}

void pxa_esp_surface_runtime_modal_leave(void) {
    bool changed = false;
    taskENTER_CRITICAL(&g_surface_lock);
    if (g_runtime_modal_count != 0) {
        --g_runtime_modal_count;
        changed = g_runtime_modal_count == 0;
        if (changed && g_surface != NULL) {
            g_direct_resume_barrier = true;
            g_direct_resume_after_frame_id = latest_frame_id_locked(g_surface);
        }
    }
    taskEXIT_CRITICAL(&g_surface_lock);
    if (changed) notify_frame_ready();
}

void pxa_esp_surface_set_host_visible(bool visible) {
    bool changed;
    taskENTER_CRITICAL(&g_surface_lock);
    changed = g_host_visible != visible;
    g_host_visible = visible;
    if (changed && visible && g_surface != NULL) {
        g_direct_resume_barrier = true;
        g_direct_resume_after_frame_id = latest_frame_id_locked(g_surface);
    }
    if (!visible) {
        g_next_input_timestamp_us = 0;
        if (g_surface != NULL) {
            g_surface->pending_input_timestamp_us = 0;
            g_surface->current_input_timestamp_us = 0;
            g_surface->acquired_input_timestamp_us = 0;
        }
    }
    taskEXIT_CRITICAL(&g_surface_lock);
    if (changed) notify_frame_ready();
}

bool pxa_esp_surface_try_resume_direct_scanout(uint64_t frame_id) {
    bool allowed;
    taskENTER_CRITICAL(&g_surface_lock);
    allowed = g_surface != NULL && frame_id != 0 && g_host_visible &&
              !g_system_overlay_visible && g_runtime_modal_count == 0 &&
              (!g_direct_resume_barrier ||
               frame_id > g_direct_resume_after_frame_id);
    if (allowed) {
        g_direct_resume_barrier = false;
        g_direct_resume_after_frame_id = 0;
    }
    taskEXIT_CRITICAL(&g_surface_lock);
    return allowed;
}

void pxa_esp_surface_note_input_sample(uint64_t timestamp_us) {
    taskENTER_CRITICAL(&g_surface_lock);
    g_next_input_timestamp_us = timestamp_us;
    taskEXIT_CRITICAL(&g_surface_lock);
}

void pxa_esp_surface_note_input_delivered(uint64_t timestamp_us,
                                          uint64_t delivered_us) {
    const uint32_t elapsed = latency_us(timestamp_us, delivered_us);
    if (timestamp_us == 0) return;
    taskENTER_CRITICAL(&g_surface_lock);
    record_latency(PXA_ESP_SURFACE_LATENCY_GUEST, elapsed,
                   &g_input_metrics.sample_to_guest_total_us,
                   &g_input_metrics.sample_to_guest_max_us,
                   &g_input_metrics.sample_to_guest_count);
    taskEXIT_CRITICAL(&g_surface_lock);
}

void pxa_esp_surface_note_frame_presented(uint64_t timestamp_us,
                                          uint64_t presented_us) {
    const uint32_t elapsed = latency_us(timestamp_us, presented_us);
    taskENTER_CRITICAL(&g_surface_lock);
    if (g_acquired_surface != NULL &&
        (g_acquired_surface->flags & PXA_ESP_SURFACE_FLAG_GAME_RENDER) != 0 &&
        g_acquired_surface->acquired >= 0) {
        const uint64_t ready_us = g_acquired_surface->raster_buffer_ready_us[
            (uint8_t)g_acquired_surface->acquired];
        if (ready_us != 0 && presented_us >= ready_us) {
            g_acquired_surface->raster_telemetry.present_us +=
                presented_us - ready_us;
            g_acquired_surface->raster_buffer_ready_us[
                (uint8_t)g_acquired_surface->acquired] = 0;
        }
    }
    if (timestamp_us != 0)
        record_latency(PXA_ESP_SURFACE_LATENCY_VISIBLE, elapsed,
                       &g_input_metrics.sample_to_visible_total_us,
                       &g_input_metrics.sample_to_visible_max_us,
                       &g_input_metrics.sample_to_visible_count);
    taskEXIT_CRITICAL(&g_surface_lock);
}

void pxa_esp_surface_take_input_metrics(
    pxa_esp_surface_input_metrics_t *metrics) {
    if (metrics == NULL) return;
    taskENTER_CRITICAL(&g_surface_lock);
    g_input_metrics.sample_to_guest_p95_us = latency_p95_us(
        PXA_ESP_SURFACE_LATENCY_GUEST,
        g_input_metrics.sample_to_guest_count,
        g_input_metrics.sample_to_guest_max_us);
    g_input_metrics.sample_to_present_p95_us = latency_p95_us(
        PXA_ESP_SURFACE_LATENCY_PRESENT,
        g_input_metrics.sample_to_present_count,
        g_input_metrics.sample_to_present_max_us);
    g_input_metrics.sample_to_visible_p95_us = latency_p95_us(
        PXA_ESP_SURFACE_LATENCY_VISIBLE,
        g_input_metrics.sample_to_visible_count,
        g_input_metrics.sample_to_visible_max_us);
    *metrics = g_input_metrics;
    memset(&g_input_metrics, 0, sizeof(g_input_metrics));
    memset(g_input_latency_histograms, 0,
           sizeof(g_input_latency_histograms));
    taskEXIT_CRITICAL(&g_surface_lock);
}

static void execute_raster_draw_list(
    const uint8_t *bytes, const pxa_raster_draw_list_view_t *list,
    const pxa_raster_target_t *target,
    const pxa_raster_resources_t *resources,
    pxa_raster_telemetry_t *telemetry, uint32_t *main_us,
    uint32_t *worker_us, uint16_t *split_row) {
    uint64_t main_started_us;
    *main_us = 0;
    *worker_us = 0;
    *split_row = target->height;
#if CONFIG_PXA_PARALLEL_RASTER
    if (g_raster_worker_task != NULL && target->height >= 2) {
        const uint16_t split = current_raster_split(target->height);
        *split_row = split;
        memset(&g_raster_worker_job, 0, sizeof(g_raster_worker_job));
        g_raster_worker_job.bytes = bytes;
        g_raster_worker_job.list = *list;
        g_raster_worker_job.target = *target;
        g_raster_worker_job.resources = *resources;
        g_raster_worker_job.row_begin = split;
        g_raster_worker_job.row_end = target->height;
        (void)xSemaphoreTake(g_raster_worker_done, 0);
        xTaskNotifyGive(g_raster_worker_task);

        main_started_us = (uint64_t)esp_timer_get_time();
        pxa_raster_execute_draw_list_rows(bytes, list, target, resources, 0,
                                          split, telemetry);
        *main_us = latency_us(main_started_us,
                              (uint64_t)esp_timer_get_time());
        (void)xSemaphoreTake(g_raster_worker_done, portMAX_DELAY);
        *worker_us = g_raster_worker_job.elapsed_us;
        telemetry->covered_pixels +=
            g_raster_worker_job.telemetry.covered_pixels;
        telemetry->last_covered_pixels +=
            g_raster_worker_job.telemetry.last_covered_pixels;
        update_raster_split(target->height, split, *main_us, *worker_us);
        return;
    }
#endif
    main_started_us = (uint64_t)esp_timer_get_time();
    pxa_raster_execute_draw_list(bytes, list, target, resources, telemetry);
    *main_us = latency_us(main_started_us, (uint64_t)esp_timer_get_time());
}

static bool materialize_latest_raster_draw(void) {
    pxa_esp_surface_t *surface;
    pxa_raster_resources_t resources;
    pxa_raster_target_t target;
    pxa_raster_draw_list_view_t list;
    pxa_raster_telemetry_t frame_telemetry;
    const uint8_t *draw_bytes;
    uint64_t frame_id;
    uint64_t input_timestamp_us;
    uint64_t queued_us;
    uint64_t started_us;
    uint64_t finished_us;
    int draw_index;
    int buffer_index = PXA_ESP_SURFACE_NONE;
    int destroy = 0;
    int log_telemetry = 0;
    uint64_t submitted_frames = 0;
    uint64_t rendered_frames = 0;
    uint64_t visible_frames = 0;
    uint64_t dropped_frames = 0;
    uint64_t queue_wait_us = 0;
    uint64_t present_us = 0;
    uint32_t raster_us = 0;
    uint32_t draw_list_bytes = 0;
    uint32_t covered_pixels = 0;
    uint32_t mailbox_capacity_0 = 0;
    uint32_t mailbox_capacity_1 = 0;
    uint32_t main_us = 0;
    uint32_t worker_us = 0;
    uint16_t split_row = 0;
    uint8_t candidate;

    taskENTER_CRITICAL(&g_surface_lock);
    surface = g_surface;
    if (surface == NULL || surface->closing ||
        (surface->flags & PXA_ESP_SURFACE_FLAG_GAME_RENDER) == 0 ||
        surface->raster_draw_pending == PXA_ESP_SURFACE_NONE ||
        surface->raster_draw_rendering != PXA_ESP_SURFACE_NONE) {
        taskEXIT_CRITICAL(&g_surface_lock);
        return false;
    }
    for (candidate = 0; candidate < surface->buffer_count; ++candidate) {
        if (buffer_is_free(surface, candidate)) {
            buffer_index = candidate;
            break;
        }
    }
    if (buffer_index == PXA_ESP_SURFACE_NONE) {
        taskEXIT_CRITICAL(&g_surface_lock);
        return false;
    }
    draw_index = surface->raster_draw_pending;
    surface->raster_draw_pending = PXA_ESP_SURFACE_NONE;
    surface->raster_draw_rendering = (int8_t)draw_index;
    surface->writing = (int8_t)buffer_index;
    ++surface->writer_active;
    draw_bytes = surface->raster_draw_lists[draw_index];
    list = surface->raster_draw_views[draw_index];
    frame_id = surface->raster_draw_frame_ids[draw_index];
    input_timestamp_us =
        surface->raster_draw_input_timestamps_us[draw_index];
    queued_us = surface->raster_draw_queued_us[draw_index];
    copy_raster_resources(surface, &resources);
    target.pixels = (uint16_t *)surface->buffers[buffer_index];
    target.depth_pixels = surface->raster_depth_buffer;
    target.stride_pixels = surface->stride_bytes / sizeof(uint16_t);
    target.depth_stride_pixels = surface->width;
    target.width = surface->width;
    target.height = surface->height;
    taskEXIT_CRITICAL(&g_surface_lock);

    started_us = (uint64_t)esp_timer_get_time();
    memset(&frame_telemetry, 0, sizeof(frame_telemetry));
    execute_raster_draw_list(draw_bytes, &list, &target, &resources,
                             &frame_telemetry, &main_us, &worker_us,
                             &split_row);
    finished_us = (uint64_t)esp_timer_get_time();
    /* The raster wrote through the cache, so push those lines out before any
     * consumer that is not this CPU reads the frame: the PPA compose and the
     * direct-scanout DMA on esp32s31-korvo-1, the panel path on the others. One
     * burst writeback per frame is far cheaper than the uncached per-pixel
     * stores it replaces, and it is what makes the cached buffer safe. */
    if (surface->buffers[buffer_index] != NULL) {
        size_t frame_bytes =
            (size_t)surface->stride_bytes * (size_t)surface->height;
        frame_bytes = (frame_bytes + 63u) & ~(size_t)63u;
        (void)esp_cache_msync(surface->buffers[buffer_index], frame_bytes,
                              ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    }

    taskENTER_CRITICAL(&g_surface_lock);
    --surface->writer_active;
    surface->raster_draw_rendering = PXA_ESP_SURFACE_NONE;
    surface->writing = PXA_ESP_SURFACE_NONE;
    if (surface->magic != PXA_ESP_SURFACE_MAGIC || surface->closing) {
        destroy = surface->magic == PXA_ESP_SURFACE_MAGIC &&
                  !surface->writer_active && !surface->acquire_active;
        taskEXIT_CRITICAL(&g_surface_lock);
        if (destroy) destroy_surface(surface);
        return false;
    }
    if (surface->pending != PXA_ESP_SURFACE_NONE) {
        ++surface->dropped_frames;
        ++surface->replaced_frames;
        ++surface->raster_telemetry.dropped_frames;
    }
    surface->pending = (int8_t)buffer_index;
    surface->pending_frame_id = frame_id;
    surface->pending_input_timestamp_us = input_timestamp_us;
    surface->raster_buffer_ready_us[buffer_index] = finished_us;
    surface->raster_telemetry.host_raster_us += finished_us - started_us;
    surface->raster_telemetry.queue_wait_us +=
        started_us >= queued_us ? started_us - queued_us : 0;
    surface->raster_telemetry.last_host_raster_us =
        finished_us - started_us > UINT32_MAX
            ? UINT32_MAX
            : (uint32_t)(finished_us - started_us);
    surface->raster_main_us = main_us;
    surface->raster_worker_us = worker_us;
    surface->raster_split_row = split_row;
    ++surface->raster_telemetry.rendered_frames;
    surface->raster_telemetry.draw_list_bytes +=
        frame_telemetry.draw_list_bytes;
    surface->raster_telemetry.covered_pixels +=
        frame_telemetry.covered_pixels;
    surface->raster_telemetry.clear_commands +=
        frame_telemetry.clear_commands;
    surface->raster_telemetry.flat_quad_commands +=
        frame_telemetry.flat_quad_commands;
    surface->raster_telemetry.textured_quad_commands +=
        frame_telemetry.textured_quad_commands;
    surface->raster_telemetry.sprite_commands +=
        frame_telemetry.sprite_commands;
    surface->raster_telemetry.last_draw_list_bytes =
        frame_telemetry.last_draw_list_bytes;
    surface->raster_telemetry.last_covered_pixels =
        frame_telemetry.last_covered_pixels;
    if (surface->raster_telemetry.rendered_frames != 0 &&
        surface->raster_telemetry.rendered_frames % UINT64_C(120) == 0) {
        log_telemetry = 1;
        submitted_frames = surface->submitted_frames;
        rendered_frames = surface->raster_telemetry.rendered_frames;
        visible_frames = surface->raster_telemetry.visible_frames;
        dropped_frames = surface->raster_telemetry.dropped_frames;
        queue_wait_us = surface->raster_telemetry.queue_wait_us;
        present_us = surface->raster_telemetry.present_us;
        raster_us = surface->raster_telemetry.last_host_raster_us;
        draw_list_bytes = surface->raster_telemetry.last_draw_list_bytes;
        covered_pixels = surface->raster_telemetry.last_covered_pixels;
        mailbox_capacity_0 = surface->raster_draw_capacities[0];
        mailbox_capacity_1 = surface->raster_draw_capacities[1];
        main_us = surface->raster_main_us;
        worker_us = surface->raster_worker_us;
        split_row = surface->raster_split_row;
    }
    taskEXIT_CRITICAL(&g_surface_lock);
    if (log_telemetry) {
        ESP_LOGI(PXA_ESP_SURFACE_TAG,
                 "Raster: submitted=%llu rendered=%llu visible=%llu "
                 "dropped=%llu list=%uB "
                 "pixels=%u raster=%uus split=%u parts=%u/%uus "
                 "queue_avg=%lluus present_avg=%lluus "
                 "mailbox=%u/%uB",
                 (unsigned long long)submitted_frames,
                 (unsigned long long)rendered_frames,
                 (unsigned long long)visible_frames,
                 (unsigned long long)dropped_frames,
                 (unsigned)draw_list_bytes, (unsigned)covered_pixels,
                 (unsigned)raster_us, (unsigned)split_row,
                 (unsigned)main_us,
                 (unsigned)worker_us,
                 (unsigned long long)(rendered_frames != 0
                                          ? queue_wait_us / rendered_frames
                                          : 0),
                 (unsigned long long)(visible_frames != 0
                                          ? present_us / visible_frames
                                          : 0),
                 (unsigned)mailbox_capacity_0,
                 (unsigned)mailbox_capacity_1);
    }
    return true;
}

static bool acquire_latest(pxa_esp_surface_frame_t *frame, bool direct_only) {
    pxa_esp_surface_t *surface;
    pxa_esp_surface_ui_alpha_provider_fn ui_alpha_provider;
    void *ui_alpha_provider_context;
    uint64_t candidate_frame_id;
    int index;
    if (frame == NULL) return false;
    memset(frame, 0, sizeof(*frame));
    if (direct_only && pxa_esp_surface_composition_required()) return false;
    (void)materialize_latest_raster_draw();
    taskENTER_CRITICAL(&g_surface_lock);
    surface = g_surface;
    if (!g_host_visible || surface == NULL || surface->closing ||
        !surface->layer.visible ||
        g_acquired_surface != NULL ||
        surface->acquire_active ||
        (surface->pending == PXA_ESP_SURFACE_NONE &&
         surface->current == PXA_ESP_SURFACE_NONE)) {
        taskEXIT_CRITICAL(&g_surface_lock);
        return false;
    }
    candidate_frame_id = surface->pending != PXA_ESP_SURFACE_NONE
                             ? surface->pending_frame_id
                             : surface->current_frame_id;
    if (direct_only &&
        (g_system_overlay_visible || g_runtime_modal_count != 0 ||
         surface->opaque_ui_region_count != 0 ||
         (g_direct_resume_barrier &&
          candidate_frame_id <= g_direct_resume_after_frame_id))) {
        taskEXIT_CRITICAL(&g_surface_lock);
        return false;
    }
    if (direct_only) {
        g_direct_resume_barrier = false;
        g_direct_resume_after_frame_id = 0;
    }
    surface->acquire_new = surface->pending != PXA_ESP_SURFACE_NONE;
    if (surface->acquire_new) {
        index = surface->pending;
        surface->pending = PXA_ESP_SURFACE_NONE;
        surface->acquired_frame_id = surface->pending_frame_id;
        surface->acquired_input_timestamp_us =
            surface->pending_input_timestamp_us;
        surface->pending_input_timestamp_us = 0;
    } else {
        index = surface->current;
        surface->acquired_frame_id = surface->current_frame_id;
        surface->acquired_input_timestamp_us =
            surface->current_input_timestamp_us;
    }
    surface->acquired = (int8_t)index;
    surface->acquire_active = 1;
    g_acquired_surface = surface;
    ++surface->acquire_generation;
    if (surface->acquire_generation == 0) ++surface->acquire_generation;
    frame->pixels = surface->buffers[index];
    frame->stride_bytes = surface->stride_bytes;
    frame->width = surface->width;
    frame->height = surface->height;
    frame->format = surface->format;
    frame->flags = surface->flags;
    frame->x = surface->layer.x;
    frame->y = surface->layer.y;
    frame->z = surface->layer.z;
    frame->visible = surface->layer.visible;
    if (g_system_overlay_visible) {
        /* The host modal is already rendered into LVGL's framebuffer. Keep
         * that framebuffer over the app Surface for the duration of the
         * modal, then restore the app's own trusted-UI regions. */
        frame->opaque_ui_region_count = 1;
        frame->opaque_ui_regions[0].x = 0;
        frame->opaque_ui_regions[0].y = 0;
        frame->opaque_ui_regions[0].width = surface->width;
        frame->opaque_ui_regions[0].height = surface->height;
    } else {
        frame->opaque_ui_region_count = surface->opaque_ui_region_count;
    }
    if (!g_system_overlay_visible && frame->opaque_ui_region_count != 0)
        memcpy(frame->opaque_ui_regions, surface->opaque_ui_regions,
               (size_t)frame->opaque_ui_region_count *
                   sizeof(frame->opaque_ui_regions[0]));
    frame->frame_id = surface->acquired_frame_id;
    frame->input_timestamp_us = surface->acquired_input_timestamp_us;
    frame->lease = surface->acquire_generation;
    ui_alpha_provider = g_ui_alpha_provider;
    ui_alpha_provider_context = g_ui_alpha_provider_context;
    taskEXIT_CRITICAL(&g_surface_lock);
    if (ui_alpha_provider != NULL &&
        !ui_alpha_provider(ui_alpha_provider_context, &frame->ui_alpha_plane))
        memset(&frame->ui_alpha_plane, 0, sizeof(frame->ui_alpha_plane));
    return true;
}

bool pxa_esp_surface_acquire_latest(pxa_esp_surface_frame_t *frame) {
    return acquire_latest(frame, false);
}

bool pxa_esp_surface_acquire_latest_for_direct(
    pxa_esp_surface_frame_t *frame) {
    return acquire_latest(frame, true);
}

bool pxa_esp_surface_has_pending_frame(void) {
    bool pending;
    taskENTER_CRITICAL(&g_surface_lock);
    pending = g_host_visible && g_surface != NULL && !g_surface->closing &&
              g_surface->layer.visible &&
              (g_surface->pending != PXA_ESP_SURFACE_NONE ||
               g_surface->raster_draw_pending != PXA_ESP_SURFACE_NONE);
    taskEXIT_CRITICAL(&g_surface_lock);
    return pending;
}

bool pxa_esp_surface_get_present_info(
    pxa_esp_surface_present_info_t *info) {
    pxa_esp_surface_t *surface;
    if (info == NULL) return false;
    taskENTER_CRITICAL(&g_surface_lock);
    surface = g_surface;
    if (surface == NULL || surface->closing ||
        surface->magic != PXA_ESP_SURFACE_MAGIC) {
        taskEXIT_CRITICAL(&g_surface_lock);
        return false;
    }
    info->width = surface->width;
    info->height = surface->height;
    info->x = surface->layer.x;
    info->y = surface->layer.y;
    info->format = surface->format;
    info->visible = surface->layer.visible && g_host_visible;
    taskEXIT_CRITICAL(&g_surface_lock);
    return true;
}

void pxa_esp_surface_release_frame(uint64_t lease) {
    pxa_esp_surface_t *surface;
    uint32_t generation =
        lease <= UINT32_MAX ? (uint32_t)lease : UINT32_C(0);
    int destroy = 0;
    int release_ready = 0;
    if (generation == 0) return;
    taskENTER_CRITICAL(&g_surface_lock);
    surface = g_acquired_surface;
    if (surface == NULL) {
        taskEXIT_CRITICAL(&g_surface_lock);
        return;
    }
    if (surface->magic != PXA_ESP_SURFACE_MAGIC ||
        !surface->acquire_active ||
        surface->acquire_generation != generation) {
        taskEXIT_CRITICAL(&g_surface_lock);
        return;
    }
    if (surface->acquire_new) {
        if ((surface->flags & PXA_SURFACE_FLAG_GUEST_MAPPED) != 0) {
            release_ready = enqueue_release_locked(
                surface, surface->acquired, surface->acquired_frame_id);
            surface->current = PXA_ESP_SURFACE_NONE;
            surface->current_frame_id = 0;
            surface->current_input_timestamp_us = 0;
        } else {
            surface->current = surface->acquired;
            surface->current_frame_id = surface->acquired_frame_id;
            surface->current_input_timestamp_us =
                surface->acquired_input_timestamp_us;
        }
        ++surface->presented_frames;
        if ((surface->flags & PXA_ESP_SURFACE_FLAG_GAME_RENDER) != 0)
            ++surface->raster_telemetry.visible_frames;
    }
    surface->acquired = PXA_ESP_SURFACE_NONE;
    surface->acquire_active = 0;
    surface->acquire_new = 0;
    surface->acquired_frame_id = 0;
    surface->acquired_input_timestamp_us = 0;
    g_acquired_surface = NULL;
    if (surface->closing && !surface->writer_active) destroy = 1;
    taskEXIT_CRITICAL(&g_surface_lock);
    if (release_ready) notify_release_ready();
    if (destroy) destroy_surface(surface);
}

#else

void pxa_esp_surface_backend(pxa_surface_backend_t *backend) {
    if (backend != NULL) backend->struct_size = 0;
}
void pxa_esp_game_render_backend(pxa_game_render_backend_t *backend) {
    if (backend != NULL) backend->struct_size = 0;
}
void pxa_esp_surface_set_frame_ready_callback(
    pxa_esp_surface_frame_ready_fn callback, void *context) {
    (void)callback;
    (void)context;
}
void pxa_esp_surface_set_release_ready_callback(
    pxa_esp_surface_frame_ready_fn callback, void *context) {
    (void)callback;
    (void)context;
}
void pxa_esp_surface_set_ui_alpha_provider(
    pxa_esp_surface_ui_alpha_provider_fn callback, void *context) {
    (void)callback;
    (void)context;
}
void pxa_esp_surface_require_composition(void) {}
bool pxa_esp_surface_composition_required(void) { return false; }
void pxa_esp_surface_set_system_overlay_visible(bool visible) {
    (void)visible;
}
void pxa_esp_surface_runtime_modal_enter(void) {}
void pxa_esp_surface_runtime_modal_leave(void) {}
void pxa_esp_surface_set_host_visible(bool visible) { (void)visible; }
bool pxa_esp_surface_try_resume_direct_scanout(uint64_t frame_id) {
    (void)frame_id;
    return false;
}
void pxa_esp_surface_note_input_sample(uint64_t timestamp_us) {
    (void)timestamp_us;
}
void pxa_esp_surface_note_input_delivered(uint64_t timestamp_us,
                                          uint64_t delivered_us) {
    (void)timestamp_us;
    (void)delivered_us;
}
void pxa_esp_surface_note_frame_presented(uint64_t timestamp_us,
                                          uint64_t presented_us) {
    (void)timestamp_us;
    (void)presented_us;
}
void pxa_esp_surface_take_input_metrics(
    pxa_esp_surface_input_metrics_t *metrics) {
    if (metrics != NULL) *metrics = (pxa_esp_surface_input_metrics_t){0};
}
bool pxa_esp_surface_acquire_latest(pxa_esp_surface_frame_t *frame) {
    (void)frame;
    return false;
}
bool pxa_esp_surface_acquire_latest_for_direct(
    pxa_esp_surface_frame_t *frame) {
    (void)frame;
    return false;
}
bool pxa_esp_surface_has_pending_frame(void) { return false; }
bool pxa_esp_surface_get_present_info(
    pxa_esp_surface_present_info_t *info) {
    (void)info;
    return false;
}
void pxa_esp_surface_release_frame(uint64_t lease) { (void)lease; }

#endif
