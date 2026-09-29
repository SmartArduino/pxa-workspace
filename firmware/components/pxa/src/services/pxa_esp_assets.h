#ifndef PXA_ESP_ASSETS_H
#define PXA_ESP_ASSETS_H

#include "pxa/assets.h"
#include "pxa/resource_budget.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* Installed Host-managed LittleFS package, never Guest private FS.
     * Metadata and directory must remain alive until end() succeeds. */
    const pxa_package_manifest_t *manifest;
    const char *package_root;
    pxa_asset_cache_config_t cache;
    size_t max_catalog_bytes;
    void *notify_context;
    void (*notify)(void *context); /* Persistent Host wakeup only, not Guest. */
    /* Optional storage-delay/failure hook, after arbitration and outside locks.
     * lane: resource=0/music=1. It must observe cancellation during injected
     * waits; a real OS read already in progress is not preemptible. */
    void *io_context;
    pxa_status_t (*before_read)(void *context, unsigned lane, size_t bytes,
        void *cancel_context, int (*cancelled)(void *));
} pxa_esp_assets_config_t;

/* One persistent, bounded worker shared by sequential package activations.
 * Activation parses the bounded installed index; pixel loading is asynchronous. */
pxa_status_t pxa_esp_assets_begin(const pxa_esp_assets_config_t *config,
                                  pxa_assets_backend_t *backend);
/* After Core shutdown closes handles/bindings: initiates cancellation, then
 * returns WOULD_BLOCK until all I/O and frame references drain. Retrying wakes
 * reclamation. OK frees activation metadata; the fixed worker stays asleep. */
pxa_status_t pxa_esp_assets_end(void);
/* Nonblocking: mark unused objects, wake worker, return planned object bytes. */
size_t pxa_esp_assets_trim(uint8_t memory_class, size_t needed_bytes);
/* Native music input. Acquire performs no I/O: validates indexed Ogg metadata,
 * pins the activation and reserves its small input state in the
 * captured allocator. Ownership transfers with the queued command; never copy
 * the owning pointer without transferring it. Release exactly once, outside
 * the audio mutex, after the decoder stops using this input. end() waits for
 * every queued/current input. Open/read/seek run only on the decoder worker. */
typedef struct pxa_esp_music_input pxa_esp_music_input_t;
pxa_status_t pxa_esp_music_input_acquire(const char *absolute_path,
    const pxa_memory_allocator_t *allocator, pxa_esp_music_input_t **input);
pxa_status_t pxa_esp_music_input_open(pxa_esp_music_input_t *input,
    void *cancel_context, int (*cancelled)(void *));
pxa_status_t pxa_esp_music_input_read(pxa_esp_music_input_t *input,
    uint8_t *output, size_t capacity, size_t *bytes);
pxa_status_t pxa_esp_music_input_rewind(pxa_esp_music_input_t *input);
int pxa_esp_music_input_eof(const pxa_esp_music_input_t *input);
void pxa_esp_music_input_release(pxa_esp_music_input_t *input);

#define PXA_ESP_ASSET_IO_RESOURCE 0u
#define PXA_ESP_ASSET_IO_MUSIC 1u
typedef struct {
    uint64_t reads, bytes, wait_us, max_wait_us, service_us, max_service_us;
    uint32_t max_read_bytes, cancellations, errors;
} pxa_esp_asset_io_lane_t;
typedef struct {
    pxa_esp_asset_io_lane_t lanes[2];
    uint8_t active_lane; /* 0=idle; otherwise lane+1. */
    uint8_t waiting[2];
} pxa_esp_asset_io_stats_t;
/* One read in flight, at most one waiting reader per lane. A waiting music
 * reader gets the next transfer. No new task, heap allocation or long lock.
 * Stats reset at begin(), and survive end() for final cancellation reporting. */
void pxa_esp_assets_io_stats(pxa_esp_asset_io_stats_t *stats);

/* Metadata includes permanent static state/mutex even after end(). Task stack
 * is configured bytes, not observed high-water use. RTOS TCB, allocator,
 * filesystem internal overhead are not included in these fields. */
void pxa_esp_assets_stats(pxa_asset_cache_stats_t *stats, size_t *metadata_bytes,
                          size_t *task_stack_bytes);

#ifdef __cplusplus
}
#endif
#endif
