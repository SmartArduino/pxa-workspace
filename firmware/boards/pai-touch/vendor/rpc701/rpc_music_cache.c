/* 曲目列表缓存实现，契约见 rpc_music_cache.h。
 *
 * 结构：每个存储设备一个槽位，曲目按下标直存一整个数组（总数已知后一次分配，
 * 优先 PSRAM），另配一张「批次是否已加载」的位图。预取任务顺序补洞，
 * get() 未命中时把该批次记为优先项，于是跳到列表末尾也不必等前面取完。
 *
 * 为什么用独立任务跑同步 RPC，而不是在异步回调里链式发下一批：
 * rpc_send_req() 的 TX 入队是 HOSTED_BLOCK_MAX 阻塞等待，在 RX 任务的回调里
 * 调它会把接收任务堵住（异步契约明确要求 cb 不得阻塞）。一个 4KB 的专用任务
 * 换来「预取全程不占用调用方任何线程」，代价小得多。
 */

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "port_esp_os.h"
#include "rpc_music_cache.h"
#include "rpc_wrap.h"

#define TAG "RPC_MUSIC_CACHE"

/* STORAGE_DEV_SD0 / SD1 / USB */
#define CACHE_DEV_CNT       3

#define CACHE_MAX_ENTRIES   (RPC_MUSIC_CACHE_MAX_BYTES / (uint32_t)sizeof(sd_music_file_info_t))

#define BATCH_OF(idx)       ((idx) / RPC_MUSIC_CACHE_BATCH)
#define BITMAP_BYTES(nb)    (((nb) + 7u) / 8u)
#define BATCH_IS_DONE(s, b) (((s)->batch_done[(b) >> 3] >> ((b) & 7u)) & 1u)
#define BATCH_SET_DONE(s, b) ((s)->batch_done[(b) >> 3] |= (uint8_t)(1u << ((b) & 7u)))

#define NO_BATCH            UINT32_MAX

typedef struct {
    sd_music_file_info_t *entries;    /* capacity 条，索引即曲目下标 */
    uint8_t              *batch_done; /* 位图，每 bit 一个批次 */
    uint32_t              capacity;   /* entries 实际容纳条数（可能 < total） */
    uint32_t              n_batches;
    uint32_t              total;      /* 701 报的总数，0 = 未知 */
    uint32_t              cached;     /* 已加载条数 */
    uint32_t              want_batch; /* get() 未命中的批次，优先取；NO_BATCH = 无 */
    rpc_music_cache_state_t state;
    uint32_t              generation; /* 每次失效 +1，用来丢弃在途的过期结果 */
    bool                  need_count; /* 还没拿到总数 */
    /* 有人要过这份数据（get/prime 调过）。跨失效保留：换卡后自动重取，
     * 也让「应用先 prime、插拔事件后到」这种顺序不再丢请求。 */
    bool                  wanted;
} cache_slot_t;

static cache_slot_t      s_slots[CACHE_DEV_CNT];
static SemaphoreHandle_t s_mtx;
static SemaphoreHandle_t s_wake;     /* 有活干时唤醒预取任务 */
static bool              s_inited;

/* ---------------- 小工具 ---------------- */

static bool dev_valid(storage_dev_type_t dev)
{
    return (uint32_t)dev < CACHE_DEV_CNT;
}

static void *cache_alloc(size_t bytes)
{
    /* 列表可能几百 KB，优先放 PSRAM；没有 PSRAM 的板子退回内部 RAM */
    void *p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        p = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
    }
    return p;
}

/* 持锁调用。释放数据并回到 EMPTY，generation++ 让在途结果作废。
 * 刻意不动 wanted —— 它表达的是「应用需要这份数据」，与数据本身无关 */
static void slot_reset_locked(cache_slot_t *slot)
{
    free(slot->entries);
    free(slot->batch_done);
    slot->entries    = NULL;
    slot->batch_done = NULL;
    slot->capacity   = 0;
    slot->n_batches  = 0;
    slot->total      = 0;
    slot->cached     = 0;
    slot->want_batch = NO_BATCH;
    slot->state      = RPC_MUSIC_CACHE_EMPTY;
    slot->need_count = false;
    slot->generation++;
}

/* 持锁调用。标记该槽位需要从头预取 */
static void slot_start_locked(cache_slot_t *slot)
{
    slot->need_count = true;
    slot->state      = RPC_MUSIC_CACHE_LOADING;
}

static void notify_progress(storage_dev_type_t dev, const cache_slot_t *slot)
{
    rpc_music_cache_status_t st = {
        .dev    = dev,
        .state  = slot->state,
        .total  = slot->total,
        .cached = slot->cached,
    };
    /* 不阻塞：事件队列满了就丢，UI 下一次 tick 还会来读缓存 */
    rpcp_event_post(RPC_VB_EVENT, VB_EVT_MUSIC_LIST_UPDATED, &st, sizeof(st), 0);
}

/* ---------------- 预取任务 ---------------- */

/* 持锁调用。挑一个待办批次：优先 get() 点名的，其次从头找第一个空洞。
 * 没有空洞返回 NO_BATCH。 */
static uint32_t pick_batch_locked(cache_slot_t *slot)
{
    if (slot->want_batch != NO_BATCH && slot->want_batch < slot->n_batches &&
        !BATCH_IS_DONE(slot, slot->want_batch)) {
        return slot->want_batch;
    }
    slot->want_batch = NO_BATCH;

    for (uint32_t b = 0; b < slot->n_batches; b++) {
        if (!BATCH_IS_DONE(slot, b)) {
            return b;
        }
    }
    return NO_BATCH;
}

/* 拿总数并分配存储。返回是否做了活 */
static bool prefetch_count(storage_dev_type_t dev, uint32_t gen)
{
    uint32_t total = 0;
    int ret = rpc_sd_music_count(dev, &total);

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    cache_slot_t *slot = &s_slots[dev];
    if (slot->generation != gen) {
        xSemaphoreGive(s_mtx);   /* 期间被失效了，结果作废 */
        return true;
    }

    if (ret != RPC_ERR_SUCCESS) {
        ESP_LOGW(TAG, "dev %d: music_count failed: %d", (int)dev, ret);
        slot->need_count = false;
        slot->state      = RPC_MUSIC_CACHE_FAILED;
        goto done;
    }

    slot->need_count = false;
    slot->total      = total;

    if (total == 0) {
        slot->state = RPC_MUSIC_CACHE_READY;   /* 空设备也是一种就绪 */
        goto done;
    }

    slot->capacity = total < CACHE_MAX_ENTRIES ? total : CACHE_MAX_ENTRIES;
    if (slot->capacity < total) {
        ESP_LOGW(TAG, "dev %d: %" PRIu32 " tracks exceed cache cap, only first %" PRIu32 " cached",
                 (int)dev, total, slot->capacity);
    }
    slot->n_batches = (slot->capacity + RPC_MUSIC_CACHE_BATCH - 1) / RPC_MUSIC_CACHE_BATCH;

    slot->entries    = cache_alloc(slot->capacity * sizeof(sd_music_file_info_t));
    slot->batch_done = calloc(1, BITMAP_BYTES(slot->n_batches));
    if (!slot->entries || !slot->batch_done) {
        ESP_LOGE(TAG, "dev %d: cache alloc failed for %" PRIu32 " tracks", (int)dev, slot->capacity);
        slot_reset_locked(slot);
        slot->state = RPC_MUSIC_CACHE_FAILED;
    }

done:;
    cache_slot_t snapshot = *slot;
    xSemaphoreGive(s_mtx);
    notify_progress(dev, &snapshot);
    return true;
}

/* 拉一个批次。返回是否做了活 */
static bool prefetch_batch(storage_dev_type_t dev, uint32_t gen, uint32_t batch)
{
    uint32_t offset = batch * RPC_MUSIC_CACHE_BATCH;
    sd_music_list_resp_t resp = {0};

    int ret = rpc_sd_music_list(dev, offset, RPC_MUSIC_CACHE_BATCH, &resp);

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    cache_slot_t *slot = &s_slots[dev];
    if (slot->generation != gen || !slot->entries) {
        xSemaphoreGive(s_mtx);
        free(resp.files);        /* 契约：files 由调用方释放 */
        return true;
    }

    if (ret != RPC_ERR_SUCCESS) {
        /* 不标记已完成：下一轮还会回来重试这个批次。留 100ms 别把 UART 打满 */
        ESP_LOGW(TAG, "dev %d: list(off=%" PRIu32 ") failed: %d", (int)dev, offset, ret);
        xSemaphoreGive(s_mtx);
        free(resp.files);
        vTaskDelay(pdMS_TO_TICKS(100));
        return true;
    }

    /* 701 报的总数变了说明卡里的内容被改过，整份作废重来 */
    if (resp.total != slot->total) {
        ESP_LOGI(TAG, "dev %d: total changed %" PRIu32 " -> %" PRIu32 ", refetching",
                 (int)dev, slot->total, resp.total);
        slot_reset_locked(slot);
        slot_start_locked(slot);
        xSemaphoreGive(s_mtx);
        free(resp.files);
        return true;
    }

    uint32_t room = slot->capacity - offset;
    uint32_t n    = resp.n_files < room ? (uint32_t)resp.n_files : room;
    if (n > 0 && resp.files) {
        memcpy(&slot->entries[offset], resp.files, n * sizeof(sd_music_file_info_t));
    }
    BATCH_SET_DONE(slot, batch);
    slot->cached += n;

    if (slot->cached >= slot->capacity) {
        slot->state = RPC_MUSIC_CACHE_READY;
        ESP_LOGI(TAG, "dev %d: cache ready, %" PRIu32 " tracks", (int)dev, slot->cached);
    }

    cache_slot_t snapshot = *slot;
    xSemaphoreGive(s_mtx);
    free(resp.files);

    notify_progress(dev, &snapshot);
    return true;
}

static void prefetch_task(void *arg)
{
    (void)arg;

    for (;;) {
        storage_dev_type_t dev = STORAGE_DEV_SD0;
        uint32_t gen = 0, batch = NO_BATCH;
        bool need_count = false, has_work = false;

        xSemaphoreTake(s_mtx, portMAX_DELAY);
        for (uint32_t d = 0; d < CACHE_DEV_CNT; d++) {
            cache_slot_t *slot = &s_slots[d];
            if (slot->state != RPC_MUSIC_CACHE_LOADING) {
                continue;
            }
            if (slot->need_count) {
                dev = (storage_dev_type_t)d;
                gen = slot->generation;
                need_count = true;
                has_work = true;
                break;
            }
            if (!slot->entries) {
                continue;        /* 分配失败过，等下次失效重来 */
            }
            batch = pick_batch_locked(slot);
            if (batch != NO_BATCH) {
                dev = (storage_dev_type_t)d;
                gen = slot->generation;
                has_work = true;
                break;
            }
            /* 没洞了却还是 LOADING：补一次收尾 */
            slot->state = RPC_MUSIC_CACHE_READY;
        }
        xSemaphoreGive(s_mtx);

        if (!has_work) {
            xSemaphoreTake(s_wake, portMAX_DELAY);
            continue;
        }

        if (need_count) {
            prefetch_count(dev, gen);
        } else {
            prefetch_batch(dev, gen, batch);
        }
    }
}

/* 清空某设备的缓存。restart=true 且此前有人要过数据时，顺带续上预取。 */
static void cache_invalidate(storage_dev_type_t dev, bool restart)
{
    if (!s_inited || !dev_valid(dev)) {
        return;
    }

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    cache_slot_t *slot = &s_slots[dev];
    slot_reset_locked(slot);
    bool kick = restart && slot->wanted;
    if (kick) {
        slot_start_locked(slot);
    }
    cache_slot_t snapshot = *slot;
    xSemaphoreGive(s_mtx);

    if (kick) {
        xSemaphoreGive(s_wake);
    }
    notify_progress(dev, &snapshot);
}

/* ---------------- 事件订阅：插拔自动失效 ---------------- */

static void storage_evt_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;

    if (id != VB_EVT_STORAGE_DEV_CHANGED || !data) {
        return;
    }
    const event_storage_dev_changed_t *e = (const event_storage_dev_changed_t *)data;
    bool inserted = (e->action == STORAGE_DEV_ACTION_INSERT);

    ESP_LOGI(TAG, "storage dev %d %s, dropping cache",
             (int)e->dev, inserted ? "inserted" : "removed");

    /* 拔出时不重取：设备已经没了，重取只会白等一次超时。
     * 插入时若之前有人要过这份数据，直接续上，应用不必再 prime 一次。 */
    cache_invalidate(e->dev, inserted);
}

/* ---------------- 对外接口 ---------------- */

int rpc_music_cache_init(void)
{
    if (s_inited) {
        return RPC_ERR_SUCCESS;
    }

    s_mtx  = xSemaphoreCreateMutex();
    s_wake = xSemaphoreCreateBinary();
    if (!s_mtx || !s_wake) {
        ESP_LOGE(TAG, "failed to create sync primitives");
        goto fail;
    }

    for (uint32_t d = 0; d < CACHE_DEV_CNT; d++) {
        s_slots[d].want_batch = NO_BATCH;
        s_slots[d].state      = RPC_MUSIC_CACHE_EMPTY;
    }

    if (esp_event_handler_register(RPC_VB_EVENT, VB_EVT_STORAGE_DEV_CHANGED,
                                   storage_evt_handler, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "failed to subscribe storage event");
        goto fail;
    }

    /* 4KB：任务里只跑同步 RPC + memcpy，曲目数组在堆上 */
    if (xTaskCreate(prefetch_task, "rpc_mcache", 4096, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create prefetch task");
        esp_event_handler_unregister(RPC_VB_EVENT, VB_EVT_STORAGE_DEV_CHANGED,
                                     storage_evt_handler);
        goto fail;
    }

    s_inited = true;
    return RPC_ERR_SUCCESS;

fail:
    if (s_mtx)  { vSemaphoreDelete(s_mtx);  s_mtx  = NULL; }
    if (s_wake) { vSemaphoreDelete(s_wake); s_wake = NULL; }
    return RPC_ERR_FAILURE;
}

int rpc_music_cache_prime(storage_dev_type_t dev)
{
    if (!s_inited) {
        return RPC_ERR_FAILURE;
    }
    if (!dev_valid(dev)) {
        return RPC_ERR_INVALID_PARAM;
    }

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    cache_slot_t *slot = &s_slots[dev];
    slot->wanted = true;
    bool kick = (slot->state == RPC_MUSIC_CACHE_EMPTY ||
                 slot->state == RPC_MUSIC_CACHE_FAILED);
    if (kick) {
        slot_start_locked(slot);
    }
    xSemaphoreGive(s_mtx);

    if (kick) {
        xSemaphoreGive(s_wake);
    }
    return RPC_ERR_SUCCESS;
}

int rpc_music_cache_get(storage_dev_type_t dev, uint32_t offset, uint32_t count,
                        sd_music_file_info_t *out, uint32_t *n_out)
{
    if (!out || !n_out) {
        return RPC_ERR_INVALID_PARAM;
    }
    *n_out = 0;

    if (!s_inited) {
        return RPC_ERR_FAILURE;
    }
    if (!dev_valid(dev)) {
        return RPC_ERR_INVALID_PARAM;
    }

    bool kick = false;

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    cache_slot_t *slot = &s_slots[dev];
    slot->wanted = true;

    if (slot->state == RPC_MUSIC_CACHE_EMPTY) {
        slot_start_locked(slot);      /* 首次访问自动暖起来 */
        kick = true;
    } else if (slot->entries && offset < slot->capacity) {
        uint32_t room = slot->capacity - offset;
        uint32_t want = count < room ? count : room;
        uint32_t n    = 0;

        /* 从 offset 起连续拷贝，遇到没加载的批次就停 */
        while (n < want && BATCH_IS_DONE(slot, BATCH_OF(offset + n))) {
            uint32_t b_end = (BATCH_OF(offset + n) + 1) * RPC_MUSIC_CACHE_BATCH - offset;
            uint32_t chunk = (b_end < want ? b_end : want) - n;
            memcpy(&out[n], &slot->entries[offset + n], chunk * sizeof(sd_music_file_info_t));
            n += chunk;
        }
        *n_out = n;

        /* 没喂满且还在预取：把缺的那批插到队首，跳页不必等前面取完 */
        if (n < want && slot->state == RPC_MUSIC_CACHE_LOADING) {
            slot->want_batch = BATCH_OF(offset + n);
            kick = true;
        }
    }
    xSemaphoreGive(s_mtx);

    if (kick) {
        xSemaphoreGive(s_wake);
    }
    return RPC_ERR_SUCCESS;
}

int rpc_music_cache_status(storage_dev_type_t dev, rpc_music_cache_status_t *out)
{
    if (!out) {
        return RPC_ERR_INVALID_PARAM;
    }
    if (!s_inited) {
        return RPC_ERR_FAILURE;
    }
    if (!dev_valid(dev)) {
        return RPC_ERR_INVALID_PARAM;
    }

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    const cache_slot_t *slot = &s_slots[dev];
    out->dev    = dev;
    out->state  = slot->state;
    out->total  = slot->total;
    out->cached = slot->cached;
    xSemaphoreGive(s_mtx);

    return RPC_ERR_SUCCESS;
}

void rpc_music_cache_invalidate(storage_dev_type_t dev)
{
    /* 手动失效意味着「数据可能变了，重新取一份」，所以续上预取 */
    cache_invalidate(dev, true);
}

void rpc_music_cache_invalidate_all(void)
{
    if (!s_inited) {
        return;
    }
    for (uint32_t d = 0; d < CACHE_DEV_CNT; d++) {
        rpc_music_cache_invalidate((storage_dev_type_t)d);
    }
}
