#ifndef PXA_ESP_SURFACE_H
#define PXA_ESP_SURFACE_H

#include <stdbool.h>
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

/* A provider is called after a Surface frame lease is acquired. Its plane must
 * remain immutable until pxa_esp_surface_release_frame() for that lease. */
typedef bool (*pxa_esp_surface_ui_alpha_provider_fn)(
    void *context, pxa_esp_surface_ui_alpha_plane_t *plane);

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

void pxa_esp_surface_backend(pxa_surface_backend_t *backend);
void pxa_esp_game_render_backend(pxa_game_render_backend_t *backend);
void pxa_esp_surface_set_frame_ready_callback(
    pxa_esp_surface_frame_ready_fn callback, void *context);
void pxa_esp_surface_set_release_ready_callback(
    pxa_esp_surface_frame_ready_fn callback, void *context);
void pxa_esp_surface_set_ui_alpha_provider(
    pxa_esp_surface_ui_alpha_provider_fn callback, void *context);
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
/* Reads immutable placement data without acquiring a framebuffer lease. */
bool pxa_esp_surface_get_present_info(
    pxa_esp_surface_present_info_t *info);
void pxa_esp_surface_release_frame(uint64_t lease);

#ifdef __cplusplus
}
#endif

#endif
