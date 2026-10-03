#ifndef PXA_ESP_SURFACE_H
#define PXA_ESP_SURFACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pxa/game_render.h"
#include "pxa/surface.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*pxa_esp_surface_frame_ready_fn)(void *context);

/* The trusted UI renderer owns these buffers. Color is RGB565 and alpha is
 * A8; alpha is applied after the Guest Surface in the panel tile worker. */
typedef struct {
    const uint16_t *pixels;
    const uint8_t *alpha;
    uint32_t pixel_stride_bytes;
    uint32_t alpha_stride_bytes;
    int32_t x;
    int32_t y;
    uint16_t width;
    uint16_t height;
    uint8_t opacity;
    uint8_t visible;
    uint64_t revision;
} pxa_esp_surface_ui_alpha_plane_t;

static inline uint16_t pxa_esp_surface_blend_alpha_pixel(
    uint16_t destination, uint16_t foreground, uint8_t alpha,
    uint8_t opacity) {
    if (alpha == 0 || opacity == 0) return destination;
    if (opacity != 255) {
        alpha = (uint8_t)(((uint16_t)alpha * opacity + 127u) / 255u);
        if (alpha == 0) return destination;
    }
    if (alpha == 255) return foreground;
    const uint16_t inverse = (uint16_t)(255u - alpha);
    const uint16_t red = (uint16_t)((((foreground >> 11) * alpha +
                                    (destination >> 11) * inverse + 128u) >> 8));
    const uint16_t green = (uint16_t)((((((foreground >> 5) & 0x3fu) * alpha +
                                      ((destination >> 5) & 0x3fu) * inverse +
                                      128u) >> 8)));
    const uint16_t blue = (uint16_t)((((foreground & 0x1fu) * alpha +
                                     (destination & 0x1fu) * inverse +
                                     128u) >> 8));
    return (uint16_t)((red << 11) | (green << 5) | blue);
}

/* A provider is called after a Surface frame lease is acquired. Its plane must
 * remain immutable until pxa_esp_surface_release_frame() for that lease. */
typedef bool (*pxa_esp_surface_ui_alpha_provider_fn)(
    void *context, pxa_esp_surface_ui_alpha_plane_t *plane);

/* Optional board accelerator for a full-frame stack of horizontal RGB565
 * bands. The callback completes synchronously before CPU rasterization. */
typedef bool (*pxa_esp_surface_fill_bands_fn)(
    void *context, uint16_t *pixels, uint32_t stride_pixels,
    uint16_t width, uint16_t height, const uint16_t *tops,
    const uint16_t *bottoms, const uint16_t *colors, uint8_t count);

typedef struct {
    const uint8_t *pixels;
    uint32_t stride_bytes;
    uint16_t width;
    uint16_t height;
    uint16_t format;
    uint8_t flags;
    int32_t x;
    int32_t y;
    int16_t z;
    uint8_t visible;
    uint8_t opaque_ui_region_count;
    pxa_surface_damage_rect_t
        opaque_ui_regions[PXA_SURFACE_MAX_OPAQUE_UI_REGIONS];
    pxa_esp_surface_ui_alpha_plane_t ui_alpha_plane;
    pxa_esp_surface_ui_alpha_plane_t system_alpha_plane;
    uint8_t suppress_guest_alpha;
    uint64_t frame_id;
    uint64_t input_timestamp_us;
    uint64_t lease;
} pxa_esp_surface_frame_t;

typedef struct {
    uint16_t width;
    uint16_t height;
    int32_t x;
    int32_t y;
    uint16_t format;
    uint8_t visible;
} pxa_esp_surface_present_info_t;

typedef struct {
    uint64_t sample_to_guest_total_us;
    uint64_t sample_to_present_total_us;
    uint64_t sample_to_visible_total_us;
    uint32_t sample_to_guest_max_us;
    uint32_t sample_to_present_max_us;
    uint32_t sample_to_visible_max_us;
    uint32_t sample_to_guest_p95_us;
    uint32_t sample_to_present_p95_us;
    uint32_t sample_to_visible_p95_us;
    uint32_t sample_to_guest_count;
    uint32_t sample_to_present_count;
    uint32_t sample_to_visible_count;
} pxa_esp_surface_input_metrics_t;

/* Opt-in, bounded diagnostic samples. The probe allocates about 2.1 KiB of PSRAM
 * only between start and clear. A full buffer reports overflow instead of
 * silently overwriting earlier frames. Raster time excludes panel transfer;
 * present interval measures time between completed new GameRender frames. */
#define PXA_ESP_SURFACE_PERF_CAPACITY 256u
typedef enum {
    PXA_ESP_SURFACE_PERF_RASTER = 0,
    PXA_ESP_SURFACE_PERF_PRESENT_INTERVAL = 1,
} pxa_esp_surface_perf_kind_t;
typedef struct {
    uint32_t raster_count;
    uint32_t present_interval_count;
    uint32_t raster_overflow;
    uint32_t present_interval_overflow;
    uint32_t list_min_bytes;
    uint32_t list_max_bytes;
    uint32_t covered_min_pixels;
    uint32_t covered_max_pixels;
    uint8_t surface_changed;
} pxa_esp_surface_perf_info_t;

/* ESP heap blocks owned directly by the Surface backend. These bytes are
 * outside the shared resource budget; counts use the allocator-reported block
 * size and include a replacement mailbox while the old one is still live. */
typedef struct {
    uint32_t frame_bytes;
    uint32_t scratch_bytes;
    uint32_t mailbox_bytes;
    uint32_t probe_bytes;
    uint32_t peak_total_bytes;
} pxa_esp_surface_memory_info_t;

void pxa_esp_surface_memory_snapshot(pxa_esp_surface_memory_info_t *info);

bool pxa_esp_surface_perf_start(void);
bool pxa_esp_surface_perf_stop(pxa_esp_surface_perf_info_t *info);
bool pxa_esp_surface_perf_read(pxa_esp_surface_perf_kind_t kind,
                               uint32_t offset, uint32_t *values,
                               uint32_t capacity, uint32_t *count);
void pxa_esp_surface_perf_clear(void);

void pxa_esp_surface_backend(pxa_surface_backend_t *backend);
void pxa_esp_game_render_backend(pxa_game_render_backend_t *backend);
/* Board presenters register their supported integer scale set before an app
 * launches. The profile applies to subsequently initialized GameRender
 * services; the default is native 1x. */
bool pxa_esp_game_render_set_scale_profile(uint8_t supported_scale_mask,
                                           uint8_t default_scale);
void pxa_esp_game_render_get_scale_profile(
    pxa_game_render_target_profile_t *profile);
void pxa_esp_surface_set_frame_ready_callback(
    pxa_esp_surface_frame_ready_fn callback, void *context);
void pxa_esp_surface_set_release_ready_callback(
    pxa_esp_surface_frame_ready_fn callback, void *context);
void pxa_esp_surface_set_ui_alpha_provider(
    pxa_esp_surface_ui_alpha_provider_fn callback, void *context);
void pxa_esp_surface_set_system_alpha_provider(
    pxa_esp_surface_ui_alpha_provider_fn callback, void *context);
struct _lv_obj_t;
/* Borrowed LVGL objects. Call on the LVGL owner thread after their state changes. */
void pxa_esp_system_overlay_set_reference_objects(
    struct _lv_obj_t *const *objects, size_t count);
void pxa_esp_surface_set_fill_bands_callback(
    pxa_esp_surface_fill_bands_fn callback, void *context);
/* Trusted LVGL content changed and must be composed above the Surface until
 * that Surface closes. Direct-mode applications must not use retained UI. */
void pxa_esp_surface_require_composition(void);
bool pxa_esp_surface_composition_required(void);
/* Temporarily compose a host-owned system overlay above the Surface. */
void pxa_esp_surface_set_system_overlay_visible(bool visible);
void pxa_esp_surface_set_power_overlay_visible(bool visible);
/* Runtime-owned modal prompts can overlap shell-managed overlays. Calls must
 * be balanced; direct scanout resumes only after the final leave and a fresh
 * complete Surface frame. */
void pxa_esp_surface_runtime_modal_enter(void);
void pxa_esp_surface_runtime_modal_leave(void);
void pxa_esp_surface_set_host_visible(bool visible);
void pxa_esp_surface_set_display_unlocked(bool unlocked);
/* A modal/visibility transition arms a barrier at the last submitted frame.
 * Direct scanout may resume only with a newer complete frame; LVGL composition
 * remains able to acquire the old frame while the barrier is armed. */
bool pxa_esp_surface_try_resume_direct_scanout(uint64_t frame_id);
void pxa_esp_surface_note_input_sample(uint64_t timestamp_us);
void pxa_esp_surface_note_input_delivered(uint64_t timestamp_us,
                                          uint64_t delivered_us);
/* Records the transfer-completion time of the first fully visible frame that
 * carries timestamp_us. */
void pxa_esp_surface_note_frame_presented(uint64_t timestamp_us,
                                          uint64_t presented_us);
/* Board presenter calls this once a GameRender frame reaches the panel. The
 * frame id deduplicates UI recompositions of the same Surface contents. */
void pxa_esp_surface_note_game_frame_presented(uint64_t frame_id,
                                                uint64_t presented_us);
void pxa_esp_surface_take_input_metrics(
    pxa_esp_surface_input_metrics_t *metrics);
bool pxa_esp_surface_acquire_latest(pxa_esp_surface_frame_t *frame);
bool pxa_esp_surface_acquire_current_for_preview(
    pxa_esp_surface_frame_t *frame);
/* Atomically acquires only a frame eligible to resume direct scanout. Unlike
 * the composition acquire path, this never consumes a modal-barrier frame. */
bool pxa_esp_surface_acquire_latest_for_direct(
    pxa_esp_surface_frame_t *frame);
bool pxa_esp_surface_has_pending_frame(void);
/* True while an application Surface owns the panel, i.e. while the compositor
 * draws application frames instead of the LVGL shell. Host overlays only need
 * their composition planes built in that state. */
bool pxa_esp_surface_has_visible_surface(void);
/* Reads immutable placement data without acquiring a framebuffer lease. */
bool pxa_esp_surface_get_present_info(
    pxa_esp_surface_present_info_t *info);
void pxa_esp_surface_release_frame(uint64_t lease);

#ifdef __cplusplus
}
#endif

#endif
