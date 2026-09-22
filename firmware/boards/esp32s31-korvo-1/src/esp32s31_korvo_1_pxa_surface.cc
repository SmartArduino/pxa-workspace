#include "esp32s31_korvo_1_pxa_surface.h"

#include <algorithm>
#include <cstring>

#include <driver/ppa.h>
#include <esp_heap_caps.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_rgb.h>
#include <esp_lvgl_port.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <pxa/pxa_esp_surface.h>
#include <pxa/pxa_surface_transform.h>
#include <pxa_board_api.h>

#include "esp32s31_korvo_1_config.h"

namespace korvo_pxa_surface {
namespace {

constexpr char kTag[] = "KorvoPxa";
constexpr uint32_t kPresenterStack = 12288;
constexpr UBaseType_t kPresenterPriority = 3;
constexpr int32_t kLockTimeoutMs = 100;
constexpr uint32_t kFrameDoneTimeoutMs = 100;
constexpr size_t kFrameBytes =
    KORVO_DISPLAY_WIDTH * KORVO_DISPLAY_HEIGHT * sizeof(uint16_t);
constexpr size_t kArgbFrameBytes =
    KORVO_DISPLAY_WIDTH * KORVO_DISPLAY_HEIGHT * sizeof(lv_color32_t);
constexpr size_t kMaxUiSpans = 16384;
constexpr uint32_t kCpuUiBlendPixelThreshold =
    KORVO_DISPLAY_WIDTH * KORVO_DISPLAY_HEIGHT / 4;

lv_display_t* g_display = nullptr;
TaskHandle_t g_presenter_task = nullptr;
esp_lcd_panel_handle_t g_panel = nullptr;
SemaphoreHandle_t g_frame_done_semaphore = nullptr;
ppa_client_handle_t g_ppa_client = nullptr;
ppa_client_handle_t g_ppa_blend_client = nullptr;
pxa_surface_alpha_span_t* g_ui_spans = nullptr;
lv_color32_t* g_ui_argb = nullptr;
uint64_t g_ui_spans_revision = 0;
uint64_t g_ui_argb_revision = 0;
int32_t g_ui_argb_x = 0;
int32_t g_ui_argb_y = 0;
uint16_t g_ui_argb_width = 0;
uint16_t g_ui_argb_height = 0;
uint8_t g_ui_argb_opacity = 0;
size_t g_ui_span_count = 0;
bool g_ui_spans_overflow = false;
uint32_t g_ui_visible_pixels = 0;
void* g_frame_buffers[3] = {};
uint8_t g_direct_buffer_index = 0;
uint8_t g_direct_displayed_index = 0;
uint8_t g_direct_pending_index = 0;
bool g_direct_has_displayed_buffer = false;
bool g_direct_submission_pending = false;
uint64_t g_direct_pending_input_timestamp_us = 0;
bool g_direct_active = false;
DirectScanoutTransitionCallback g_transition_callback = nullptr;
void* g_transition_context = nullptr;
uint64_t g_last_direct_frame_id = 0;
lv_area_t g_last_present_area = {};
bool g_has_last_present_area = false;

int64_t g_compose_report_started_us = 0;
uint32_t g_compose_frames = 0;
uint32_t g_compose_ppa_frames = 0;
uint32_t g_compose_ui_ppa_frames = 0;
uint64_t g_compose_total_us = 0;
uint32_t g_compose_max_us = 0;
uint64_t g_compose_ui_total_us = 0;
uint32_t g_compose_ui_max_us = 0;
uint64_t g_compose_ui_pixels = 0;

int64_t g_direct_report_started_us = 0;
uint32_t g_direct_frames = 0;
uint64_t g_direct_compose_total_us = 0;
uint32_t g_direct_compose_max_us = 0;
uint64_t g_direct_wait_total_us = 0;
uint32_t g_direct_wait_max_us = 0;
uint32_t g_direct_timeouts = 0;

bool AreasEqual(const lv_area_t& lhs, const lv_area_t& rhs) {
    return lhs.x1 == rhs.x1 && lhs.y1 == rhs.y1 && lhs.x2 == rhs.x2 &&
           lhs.y2 == rhs.y2;
}

struct SurfaceGeometry {
    int32_t origin_x;
    int32_t origin_y;
    int32_t width;
    int32_t height;
    uint8_t scale;
};

SurfaceGeometry ResolveGeometry(uint16_t width, uint16_t height, int32_t x,
                                int32_t y) {
    uint8_t scale = pxa_surface_fit_scale(
        width, height, KORVO_DISPLAY_WIDTH, KORVO_DISPLAY_HEIGHT);
    if (scale == 0) scale = 1;
    const int32_t drawn_width = static_cast<int32_t>(width) * scale;
    const int32_t drawn_height = static_cast<int32_t>(height) * scale;
    if (x == 0 && y == 0 &&
        (drawn_width < KORVO_DISPLAY_WIDTH ||
         drawn_height < KORVO_DISPLAY_HEIGHT)) {
        x = (KORVO_DISPLAY_WIDTH - drawn_width) / 2;
        y = (KORVO_DISPLAY_HEIGHT - drawn_height) / 2;
    }
    return {x, y, drawn_width, drawn_height, scale};
}

bool ResolvePresentArea(lv_area_t* area) {
    pxa_esp_surface_present_info_t info;
    if (area == nullptr || !pxa_esp_surface_get_present_info(&info) ||
        !info.visible)
        return false;
    const SurfaceGeometry geometry = ResolveGeometry(
        info.width, info.height, info.x, info.y);
    area->x1 = static_cast<lv_coord_t>(
        std::max<int32_t>(geometry.origin_x, 0));
    area->y1 = static_cast<lv_coord_t>(
        std::max<int32_t>(geometry.origin_y, 0));
    area->x2 = static_cast<lv_coord_t>(std::min<int32_t>(
        geometry.origin_x + geometry.width, KORVO_DISPLAY_WIDTH) - 1);
    area->y2 = static_cast<lv_coord_t>(std::min<int32_t>(
        geometry.origin_y + geometry.height, KORVO_DISPLAY_HEIGHT) - 1);
    return area->x1 <= area->x2 && area->y1 <= area->y2;
}

/* A Surface can take the panel directly only when it fills the whole panel at
 * an exact integer scale with no trusted-UI regions, no alpha plane and no Host
 * overlay or modal that needs LVGL composition. */
bool DirectPathViable() {
    pxa_esp_surface_present_info_t info;
    if (pxa_esp_surface_composition_required() ||
        !pxa_esp_surface_get_present_info(&info) || !info.visible ||
        info.format != PXA_SURFACE_FORMAT_RGB565 || info.x != 0 || info.y != 0 ||
        info.width == 0 || info.height == 0)
        return false;
    const uint8_t scale = pxa_surface_fit_scale(
        info.width, info.height, KORVO_DISPLAY_WIDTH, KORVO_DISPLAY_HEIGHT);
    return scale != 0 && info.width * scale == KORVO_DISPLAY_WIDTH &&
           info.height * scale == KORVO_DISPLAY_HEIGHT;
}

bool DirectFrameEligible(const pxa_esp_surface_frame_t& frame) {
    if (frame.format != PXA_SURFACE_FORMAT_RGB565 || !frame.visible ||
        frame.opaque_ui_region_count != 0 || frame.ui_alpha_plane.visible ||
        frame.x != 0 || frame.y != 0 || frame.width == 0 || frame.height == 0)
        return false;
    const uint8_t scale = pxa_surface_fit_scale(
        frame.width, frame.height, KORVO_DISPLAY_WIDTH, KORVO_DISPLAY_HEIGHT);
    return scale != 0 && frame.width * scale == KORVO_DISPLAY_WIDTH &&
           frame.height * scale == KORVO_DISPLAY_HEIGHT;
}

struct Span {
    int32_t left;
    int32_t right;
};

void CopyScaledSpan(const uint16_t* source, uint16_t* destination,
                    int32_t left, int32_t right, int32_t origin_x,
                    uint8_t scale) {
    if (left >= right) return;
    if (scale == 1) {
        std::memcpy(destination + left, source + left - origin_x,
                    static_cast<size_t>(right - left) * sizeof(uint16_t));
        return;
    }
    if (scale == 2) {
        int32_t x = left;
        if (((x - origin_x) & 1) != 0) {
            destination[x] = source[(x - origin_x) >> 1];
            ++x;
        }
        while (x + 1 < right) {
            const uint32_t value = source[(x - origin_x) >> 1];
            const uint32_t doubled = value | (value << 16);
            std::memcpy(destination + x, &doubled, sizeof(doubled));
            x += 2;
        }
        if (x < right) destination[x] = source[(x - origin_x) >> 1];
        return;
    }
    for (int32_t x = left; x < right; ++x)
        destination[x] = source[(x - origin_x) / scale];
}

/* The buffer is a whole RGB panel framebuffer. Convert opaque UI rectangles
 * into row spans so the common 2x path does no division or rectangle scan per
 * output pixel. */
void ComposeRgb565(const pxa_esp_surface_frame_t& frame, uint8_t* pixels) {
    if (pixels == nullptr || frame.pixels == nullptr || !frame.visible) return;
    const SurfaceGeometry geometry = ResolveGeometry(
        frame.width, frame.height, frame.x, frame.y);
    const int32_t left = std::max<int32_t>(geometry.origin_x, 0);
    const int32_t right = std::min<int32_t>(
        geometry.origin_x + geometry.width, KORVO_DISPLAY_WIDTH);
    const int32_t top = std::max<int32_t>(geometry.origin_y, 0);
    const int32_t bottom = std::min<int32_t>(
        geometry.origin_y + geometry.height, KORVO_DISPLAY_HEIGHT);
    if (left >= right || top >= bottom) return;

    for (int32_t y = top; y < bottom; ++y) {
        const int32_t source_y =
            (y - geometry.origin_y) / geometry.scale;
        const auto* source = reinterpret_cast<const uint16_t*>(
            frame.pixels + static_cast<size_t>(source_y) * frame.stride_bytes);
        auto* destination = reinterpret_cast<uint16_t*>(pixels) +
            static_cast<size_t>(y) * KORVO_DISPLAY_WIDTH;
        Span opaque[PXA_SURFACE_MAX_OPAQUE_UI_REGIONS];
        uint8_t opaque_count = 0;
        for (uint8_t index = 0; index < frame.opaque_ui_region_count; ++index) {
            const auto& rect = frame.opaque_ui_regions[index];
            if (source_y < rect.y ||
                source_y >= static_cast<int32_t>(rect.y) + rect.height)
                continue;
            Span span = {
                std::max<int32_t>(left, geometry.origin_x +
                    static_cast<int32_t>(rect.x) * geometry.scale),
                std::min<int32_t>(right, geometry.origin_x +
                    (static_cast<int32_t>(rect.x) + rect.width) *
                        geometry.scale),
            };
            if (span.left >= span.right) continue;
            uint8_t position = opaque_count;
            while (position > 0 && opaque[position - 1].left > span.left) {
                opaque[position] = opaque[position - 1];
                --position;
            }
            opaque[position] = span;
            ++opaque_count;
        }

        int32_t cursor = left;
        for (uint8_t index = 0; index < opaque_count; ++index) {
            CopyScaledSpan(source, destination, cursor,
                           std::max(cursor, opaque[index].left),
                           geometry.origin_x, geometry.scale);
            cursor = std::max(cursor, opaque[index].right);
        }
        CopyScaledSpan(source, destination, cursor, right,
                       geometry.origin_x, geometry.scale);
    }
}

bool TryPpaScaleToBuffer(const pxa_esp_surface_frame_t& frame,
                         uint8_t* target) {
    if (g_ppa_client == nullptr || target == nullptr ||
        frame.pixels == nullptr || !frame.visible ||
        frame.format != PXA_SURFACE_FORMAT_RGB565 ||
        frame.stride_bytes != static_cast<uint32_t>(frame.width) *
                                  sizeof(uint16_t) ||
        frame.opaque_ui_region_count != 0)
        return false;
    const SurfaceGeometry geometry = ResolveGeometry(
        frame.width, frame.height, frame.x, frame.y);
    if (geometry.origin_x < 0 || geometry.origin_y < 0 ||
        geometry.origin_x + geometry.width > KORVO_DISPLAY_WIDTH ||
        geometry.origin_y + geometry.height > KORVO_DISPLAY_HEIGHT)
        return false;
    const ppa_srm_oper_config_t config = {
        .in = {.buffer = const_cast<uint8_t*>(frame.pixels),
               .pic_w = frame.width, .pic_h = frame.height,
               .block_w = frame.width, .block_h = frame.height,
               .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
        .out = {.buffer = target, .buffer_size = kFrameBytes,
                .pic_w = KORVO_DISPLAY_WIDTH, .pic_h = KORVO_DISPLAY_HEIGHT,
                .block_offset_x = static_cast<uint32_t>(geometry.origin_x),
                .block_offset_y = static_cast<uint32_t>(geometry.origin_y),
                .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = static_cast<float>(geometry.scale),
        .scale_y = static_cast<float>(geometry.scale),
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    return ppa_do_scale_rotate_mirror(g_ppa_client, &config) == ESP_OK;
}

uint8_t Expand5(uint16_t value) {
    return static_cast<uint8_t>((value << 3) | (value >> 2));
}

uint8_t Expand6(uint16_t value) {
    return static_cast<uint8_t>((value << 2) | (value >> 4));
}

bool RefreshUiSpans(const pxa_esp_surface_ui_alpha_plane_t& plane) {
    if (g_ui_spans == nullptr || plane.revision == 0 ||
        plane.alpha == nullptr || plane.alpha_stride_bytes < plane.width)
        return false;
    if (plane.revision == g_ui_spans_revision)
        return !g_ui_spans_overflow;

    g_ui_span_count = pxa_surface_alpha_spans(
        plane.alpha, plane.width, plane.height, plane.alpha_stride_bytes,
        g_ui_spans, kMaxUiSpans);
    g_ui_spans_overflow = g_ui_span_count == SIZE_MAX;
    g_ui_visible_pixels = 0;
    if (g_ui_spans_overflow) {
        g_ui_span_count = 0;
        ESP_LOGW(kTag, "UI span capacity exceeded; scanning alpha plane");
    } else {
        for (size_t index = 0; index < g_ui_span_count; ++index)
            g_ui_visible_pixels += g_ui_spans[index].width;
    }
    g_ui_spans_revision = plane.revision;
    return !g_ui_spans_overflow;
}

void ClearUiArgbCache() {
    if (g_ui_argb == nullptr || g_ui_argb_revision == 0) return;
    if (g_ui_spans == nullptr || g_ui_spans_overflow ||
        g_ui_spans_revision != g_ui_argb_revision) {
        std::memset(g_ui_argb, 0, kArgbFrameBytes);
        return;
    }
    for (size_t index = 0; index < g_ui_span_count; ++index) {
        const pxa_surface_alpha_span_t& span = g_ui_spans[index];
        const int32_t target_y =
            g_ui_argb_y + static_cast<int32_t>(span.y);
        if (target_y < 0 || target_y >= KORVO_DISPLAY_HEIGHT) continue;
        const int32_t left = std::max<int32_t>(
            g_ui_argb_x + static_cast<int32_t>(span.x), 0);
        const int32_t right = std::min<int32_t>(
            g_ui_argb_x + static_cast<int32_t>(span.x) + span.width,
            KORVO_DISPLAY_WIDTH);
        if (left >= right) continue;
        std::memset(g_ui_argb +
                        static_cast<size_t>(target_y) * KORVO_DISPLAY_WIDTH +
                        left,
                    0, static_cast<size_t>(right - left) *
                           sizeof(lv_color32_t));
    }
}

bool PrepareUiArgb(const pxa_esp_surface_ui_alpha_plane_t& plane) {
    if (g_ui_argb == nullptr || plane.revision == 0 ||
        plane.pixels == nullptr || plane.alpha == nullptr ||
        plane.pixel_stride_bytes % sizeof(uint16_t) != 0 ||
        plane.pixel_stride_bytes / sizeof(uint16_t) < plane.width ||
        plane.alpha_stride_bytes < plane.width)
        return false;
    if (g_ui_argb_revision == plane.revision &&
        g_ui_argb_x == plane.x && g_ui_argb_y == plane.y &&
        g_ui_argb_width == plane.width &&
        g_ui_argb_height == plane.height &&
        g_ui_argb_opacity == plane.opacity)
        return true;

    ClearUiArgbCache();
    const uint32_t color_stride =
        plane.pixel_stride_bytes / sizeof(uint16_t);
    if (RefreshUiSpans(plane)) {
        for (size_t index = 0; index < g_ui_span_count; ++index) {
            const pxa_surface_alpha_span_t& span = g_ui_spans[index];
            const int32_t target_y =
                plane.y + static_cast<int32_t>(span.y);
            if (target_y < 0 || target_y >= KORVO_DISPLAY_HEIGHT) continue;
            const int32_t first_x = std::max<int32_t>(
                span.x, -plane.x);
            const int32_t last_x = std::min<int32_t>(
                static_cast<int32_t>(span.x) + span.width,
                KORVO_DISPLAY_WIDTH - plane.x);
            if (first_x >= last_x) continue;
            const uint16_t* const colors =
                plane.pixels + static_cast<size_t>(span.y) * color_stride;
            const uint8_t* const alpha =
                plane.alpha + static_cast<size_t>(span.y) *
                                  plane.alpha_stride_bytes;
            lv_color32_t* const output =
                g_ui_argb + static_cast<size_t>(target_y) *
                                KORVO_DISPLAY_WIDTH;
            for (int32_t source_x = first_x; source_x < last_x; ++source_x) {
                uint8_t effective_alpha = alpha[source_x];
                if (plane.opacity != 255) {
                    effective_alpha = static_cast<uint8_t>(
                        (static_cast<uint16_t>(effective_alpha) *
                             plane.opacity +
                         127U) /
                        255U);
                }
                if (effective_alpha == 0) continue;
                const uint16_t color = colors[source_x];
                lv_color32_t& pixel = output[plane.x + source_x];
                pixel.red = Expand5(color >> 11);
                pixel.green = Expand6((color >> 5) & 0x3fU);
                pixel.blue = Expand5(color & 0x1fU);
                pixel.alpha = effective_alpha;
            }
        }
    } else {
        std::memset(g_ui_argb, 0, kArgbFrameBytes);
        g_ui_visible_pixels = 0;
        for (uint32_t source_y = 0; source_y < plane.height; ++source_y) {
            const int32_t target_y = plane.y + static_cast<int32_t>(source_y);
            if (target_y < 0 || target_y >= KORVO_DISPLAY_HEIGHT) continue;
            const uint16_t* const colors =
                plane.pixels + static_cast<size_t>(source_y) * color_stride;
            const uint8_t* const alpha =
                plane.alpha + static_cast<size_t>(source_y) *
                                  plane.alpha_stride_bytes;
            lv_color32_t* const output =
                g_ui_argb + static_cast<size_t>(target_y) *
                                KORVO_DISPLAY_WIDTH;
            for (uint32_t source_x = 0; source_x < plane.width; ++source_x) {
                const int32_t target_x =
                    plane.x + static_cast<int32_t>(source_x);
                if (target_x < 0 || target_x >= KORVO_DISPLAY_WIDTH) continue;
                uint8_t effective_alpha = alpha[source_x];
                if (effective_alpha == 0) continue;
                ++g_ui_visible_pixels;
                if (plane.opacity != 255) {
                    effective_alpha = static_cast<uint8_t>(
                        (static_cast<uint16_t>(effective_alpha) *
                             plane.opacity +
                         127U) /
                        255U);
                }
                if (effective_alpha == 0) continue;
                const uint16_t color = colors[source_x];
                lv_color32_t& pixel = output[target_x];
                pixel.red = Expand5(color >> 11);
                pixel.green = Expand6((color >> 5) & 0x3fU);
                pixel.blue = Expand5(color & 0x1fU);
                pixel.alpha = effective_alpha;
            }
        }
    }
    g_ui_argb_revision = plane.revision;
    g_ui_argb_x = plane.x;
    g_ui_argb_y = plane.y;
    g_ui_argb_width = plane.width;
    g_ui_argb_height = plane.height;
    g_ui_argb_opacity = plane.opacity;
    return true;
}

bool TryPpaBlendUi(const pxa_esp_surface_frame_t& frame, uint8_t* target) {
    const auto& plane = frame.ui_alpha_plane;
    if (g_ppa_blend_client == nullptr || target == nullptr ||
        !plane.visible || plane.opacity == 0)
        return false;
    /* PPA blend always reads and writes the complete panel. Sparse overlays
     * are cheaper to blend directly from their cached nontransparent spans. */
    if (RefreshUiSpans(plane) &&
        g_ui_visible_pixels <= kCpuUiBlendPixelThreshold)
        return false;
    if (!PrepareUiArgb(plane))
        return false;
    const ppa_blend_oper_config_t config = {
        .in_bg = {.buffer = target,
                  .pic_w = KORVO_DISPLAY_WIDTH,
                  .pic_h = KORVO_DISPLAY_HEIGHT,
                  .block_w = KORVO_DISPLAY_WIDTH,
                  .block_h = KORVO_DISPLAY_HEIGHT,
                  .blend_cm = PPA_BLEND_COLOR_MODE_RGB565},
        .in_fg = {.buffer = g_ui_argb,
                  .pic_w = KORVO_DISPLAY_WIDTH,
                  .pic_h = KORVO_DISPLAY_HEIGHT,
                  .block_w = KORVO_DISPLAY_WIDTH,
                  .block_h = KORVO_DISPLAY_HEIGHT,
                  .blend_cm = PPA_BLEND_COLOR_MODE_ARGB8888},
        .out = {.buffer = target,
                .buffer_size = kFrameBytes,
                .pic_w = KORVO_DISPLAY_WIDTH,
                .pic_h = KORVO_DISPLAY_HEIGHT,
                .blend_cm = PPA_BLEND_COLOR_MODE_RGB565},
        .bg_alpha_update_mode = PPA_ALPHA_NO_CHANGE,
        .fg_alpha_update_mode = PPA_ALPHA_NO_CHANGE,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    return ppa_do_blend(g_ppa_blend_client, &config) == ESP_OK;
}

uint32_t BlendUiAlphaPlane(const pxa_esp_surface_frame_t& frame,
                           uint8_t* target) {
    const auto& plane = frame.ui_alpha_plane;
    if (target == nullptr || !plane.visible || plane.opacity == 0 ||
        plane.pixels == nullptr || plane.alpha == nullptr ||
        plane.pixel_stride_bytes % sizeof(uint16_t) != 0)
        return 0;
    if (g_ui_spans != nullptr && plane.revision != 0 &&
        plane.pixel_stride_bytes / sizeof(uint16_t) >= plane.width &&
        plane.alpha_stride_bytes >= plane.width) {
        if (RefreshUiSpans(plane)) {
            uint32_t blended = 0;
            const uint32_t color_stride =
                plane.pixel_stride_bytes / sizeof(uint16_t);
            for (size_t index = 0; index < g_ui_span_count; ++index) {
                const pxa_surface_alpha_span_t& span = g_ui_spans[index];
                const size_t color_offset =
                    static_cast<size_t>(span.y) * color_stride + span.x;
                const size_t alpha_offset =
                    static_cast<size_t>(span.y) *
                        plane.alpha_stride_bytes + span.x;
                blended += pxa_surface_blend_rgb565_a8(
                    reinterpret_cast<uint16_t*>(target),
                    KORVO_DISPLAY_WIDTH, KORVO_DISPLAY_HEIGHT,
                    KORVO_DISPLAY_WIDTH, plane.pixels + color_offset,
                    plane.alpha + alpha_offset, span.width, 1,
                    span.width, span.width, plane.x + span.x,
                    plane.y + span.y, plane.opacity);
            }
            return blended;
        }
    }
    return pxa_surface_blend_rgb565_a8(
        reinterpret_cast<uint16_t*>(target), KORVO_DISPLAY_WIDTH,
        KORVO_DISPLAY_HEIGHT, KORVO_DISPLAY_WIDTH, plane.pixels, plane.alpha,
        plane.width, plane.height,
        plane.pixel_stride_bytes / sizeof(uint16_t),
        plane.alpha_stride_bytes, plane.x, plane.y, plane.opacity);
}

void ComposeFullFrame(const pxa_esp_surface_frame_t& frame, uint8_t* target) {
    if (!TryPpaScaleToBuffer(frame, target)) ComposeRgb565(frame, target);
    if (!TryPpaBlendUi(frame, target))
        (void)BlendUiAlphaPlane(frame, target);
}

void SetRefreshPaused(bool paused) {
    if (g_display == nullptr) return;
    lv_timer_t* refresh_timer = lv_display_get_refr_timer(g_display);
    if (refresh_timer == nullptr) return;
    if (paused)
        lv_timer_pause(refresh_timer);
    else
        lv_timer_resume(refresh_timer);
}

bool CompletePendingDirectSubmission(uint32_t* wait_us) {
    if (wait_us != nullptr) *wait_us = 0;
    if (!g_direct_submission_pending) return true;
    const int64_t started_us = esp_timer_get_time();
    const bool frame_done =
        xSemaphoreTake(g_frame_done_semaphore,
                       pdMS_TO_TICKS(kFrameDoneTimeoutMs)) == pdTRUE;
    const int64_t finished_us = esp_timer_get_time();
    if (wait_us != nullptr) {
        *wait_us = static_cast<uint32_t>(finished_us - started_us);
    }
    if (!frame_done) {
        ++g_direct_timeouts;
        return false;
    }
    g_direct_displayed_index = g_direct_pending_index;
    g_direct_has_displayed_buffer = true;
    g_direct_submission_pending = false;
    pxa_esp_surface_note_frame_presented(
        g_direct_pending_input_timestamp_us,
        static_cast<uint64_t>(finished_us));
    return true;
}

bool PresentDirectFrame(const pxa_esp_surface_frame_t& frame) {
    if (g_panel == nullptr || g_frame_buffers[0] == nullptr ||
        g_frame_buffers[1] == nullptr || g_frame_buffers[2] == nullptr)
        return false;
    uint8_t* target = static_cast<uint8_t*>(
        g_frame_buffers[g_direct_buffer_index]);
    /* Triple buffering leaves this target independent from both the live
     * scanout buffer and the pending buffer. Compose first so LCD scanout and
     * PPA/CPU composition overlap, then wait before queueing another switch. */
    const int64_t compose_started_us = esp_timer_get_time();
    ComposeFullFrame(frame, target);
    pxa_board_performance_note_frame();
    pxa_board_performance_draw_rgb565(
        reinterpret_cast<uint16_t*>(target), KORVO_DISPLAY_WIDTH,
        KORVO_DISPLAY_HEIGHT, KORVO_DISPLAY_WIDTH, 0, 0,
        KORVO_DISPLAY_WIDTH, KORVO_DISPLAY_HEIGHT, false);
    const int64_t compose_finished_us = esp_timer_get_time();
    uint32_t wait_us = 0;
    if (!CompletePendingDirectSubmission(&wait_us)) {
        ESP_LOGW(kTag, "Direct scanout frame completion timed out");
        return false;
    }
    /* Consume callbacks left by the idle scan before queuing this switch.
     * Draining after draw_bitmap() can erase the completion notification for
     * the switch that was just queued, leaving the triple-buffer pipeline
     * stalled until a later link-switch event. */
    (void)xSemaphoreTake(g_frame_done_semaphore, 0);
    if (esp_lcd_panel_draw_bitmap(g_panel, 0, 0, KORVO_DISPLAY_WIDTH,
                                  KORVO_DISPLAY_HEIGHT,
                                  target) != ESP_OK) {
        ESP_LOGW(kTag, "Direct scanout buffer switch failed");
        return false;
    }
    g_direct_pending_index = g_direct_buffer_index;
    g_direct_pending_input_timestamp_us = frame.input_timestamp_us;
    g_direct_submission_pending = true;
    g_direct_buffer_index = (g_direct_buffer_index + 1U) % 3U;

    const uint32_t compose_us =
        static_cast<uint32_t>(compose_finished_us - compose_started_us);
    ++g_direct_frames;
    g_direct_compose_total_us += compose_us;
    g_direct_compose_max_us = std::max(g_direct_compose_max_us, compose_us);
    g_direct_wait_total_us += wait_us;
    g_direct_wait_max_us = std::max(g_direct_wait_max_us, wait_us);
    const int64_t now_us = esp_timer_get_time();
    if (g_direct_report_started_us == 0) g_direct_report_started_us = now_us;
    if (now_us - g_direct_report_started_us >= 1000 * 1000) {
        ESP_LOGI(kTag,
                 "PERF direct=%u compose=%u/%u ms wait=%u/%u ms timeout=%u",
                 g_direct_frames,
                 static_cast<uint32_t>(
                     g_direct_compose_total_us / g_direct_frames) / 1000,
                 g_direct_compose_max_us / 1000,
                 static_cast<uint32_t>(
                     g_direct_wait_total_us / g_direct_frames) / 1000,
                 g_direct_wait_max_us / 1000, g_direct_timeouts);
        g_direct_report_started_us = now_us;
        g_direct_frames = 0;
        g_direct_compose_total_us = 0;
        g_direct_compose_max_us = 0;
        g_direct_wait_total_us = 0;
        g_direct_wait_max_us = 0;
        g_direct_timeouts = 0;
    }
    return true;
}

bool EnterDirectScanout() {
    if (g_direct_active) return true;
    if (!lvgl_port_lock(kLockTimeoutMs)) return false;
    SetRefreshPaused(true);
    if (g_transition_callback != nullptr &&
        !g_transition_callback(true, nullptr, nullptr,
                               g_transition_context)) {
        SetRefreshPaused(false);
        lvgl_port_unlock();
        ESP_LOGW(kTag, "Direct scanout transition timed out");
        return false;
    }
    lv_draw_buf_t* const active = lv_display_get_buf_active(g_display);
    uint8_t next_index = 3;
    for (uint8_t index = 0; index < 3; ++index) {
        if (active != nullptr && active->data == g_frame_buffers[index]) {
            next_index = index;
            break;
        }
    }
    if (next_index >= 3) {
        SetRefreshPaused(false);
        lvgl_port_unlock();
        ESP_LOGE(kTag, "Cannot resolve direct scanout start buffer");
        return false;
    }
    g_direct_active = true;
    g_direct_buffer_index = next_index;
    g_direct_has_displayed_buffer = false;
    g_direct_submission_pending = false;
    g_direct_pending_input_timestamp_us = 0;
    g_last_direct_frame_id = 0;
    lvgl_port_unlock();
    ESP_LOGI(kTag, "Direct scanout engaged");
    return true;
}

void ExitDirectScanout() {
    if (!g_direct_active) return;
    if (!lvgl_port_lock(kLockTimeoutMs)) return;
    uint32_t wait_us = 0;
    if (!CompletePendingDirectSubmission(&wait_us)) {
        lvgl_port_unlock();
        ESP_LOGW(kTag, "Keeping direct scanout after completion timeout");
        if (g_presenter_task != nullptr) xTaskNotifyGive(g_presenter_task);
        return;
    }
    if (g_transition_callback != nullptr && g_direct_has_displayed_buffer) {
        (void)g_transition_callback(
            false, g_frame_buffers[g_direct_buffer_index],
            g_frame_buffers[g_direct_displayed_index], g_transition_context);
    }
    g_direct_active = false;
    SetRefreshPaused(false);
    lv_obj_t* screen = lv_display_get_screen_active(g_display);
    if (screen != nullptr) {
        lv_obj_invalidate(screen);
        g_has_last_present_area = false;
    }
    lvgl_port_unlock();
    ESP_LOGI(kTag, "Direct scanout released");
}

void InvalidatePresentArea() {
    if (!lvgl_port_lock(kLockTimeoutMs)) return;
    lv_obj_t* screen = lv_display_get_screen_active(g_display);
    lv_area_t area;
    if (screen != nullptr && ResolvePresentArea(&area)) {
        const bool geometry_changed =
            !g_has_last_present_area || !AreasEqual(g_last_present_area, area);
        if (g_has_last_present_area && geometry_changed)
            (void)lv_obj_invalidate_area(screen, &g_last_present_area);
        if (geometry_changed) {
            /* Rebuild the LVGL background and trusted UI when a Surface first
             * appears or changes placement. */
            (void)lv_obj_invalidate_area(screen, &area);
        } else {
            /* The final flush composites the complete latest Surface. A
             * one-pixel invalidation is enough to schedule that flush while
             * preserving LVGL's normal invalidations for changed UI. */
            lv_area_t trigger = {area.x1, area.y1, area.x1, area.y1};
            (void)lv_obj_invalidate_area(screen, &trigger);
        }
        g_last_present_area = area;
        g_has_last_present_area = true;
    } else if (screen != nullptr && g_has_last_present_area) {
        (void)lv_obj_invalidate_area(screen, &g_last_present_area);
        g_has_last_present_area = false;
    }
    lvgl_port_unlock();
}

void FrameReady(void*) {
    if (g_presenter_task != nullptr) xTaskNotifyGive(g_presenter_task);
}

void PresenterTask(void*) {
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (g_display == nullptr) continue;

        if (g_direct_active) {
            if (!DirectPathViable()) {
                ExitDirectScanout();
                InvalidatePresentArea();
                continue;
            }
            pxa_esp_surface_frame_t frame;
            if (!pxa_esp_surface_acquire_latest_for_direct(&frame)) continue;
            const bool eligible = DirectFrameEligible(frame);
            const bool is_new = frame.frame_id != g_last_direct_frame_id;
            if (eligible && is_new) {
                if (PresentDirectFrame(frame))
                    g_last_direct_frame_id = frame.frame_id;
                pxa_esp_surface_release_frame(frame.lease);
                continue;
            }
            pxa_esp_surface_release_frame(frame.lease);
            if (!eligible) {
                ExitDirectScanout();
                InvalidatePresentArea();
            }
            continue;
        }

        if (DirectPathViable()) {
            pxa_esp_surface_frame_t frame;
            if (pxa_esp_surface_acquire_latest_for_direct(&frame)) {
                if (DirectFrameEligible(frame) && EnterDirectScanout()) {
                    if (PresentDirectFrame(frame))
                        g_last_direct_frame_id = frame.frame_id;
                    pxa_esp_surface_release_frame(frame.lease);
                    continue;
                }
                pxa_esp_surface_release_frame(frame.lease);
            }
        }
        InvalidatePresentArea();
    }
}

}  // namespace

bool Install(lv_display_t* display, esp_lcd_panel_handle_t panel,
             SemaphoreHandle_t frame_done_semaphore,
             DirectScanoutTransitionCallback transition_callback,
             void* transition_context) {
    if (display == nullptr || panel == nullptr ||
        frame_done_semaphore == nullptr || g_display != nullptr)
        return false;
    if (esp_lcd_rgb_panel_get_frame_buffer(
            panel, 3, &g_frame_buffers[0], &g_frame_buffers[1],
            &g_frame_buffers[2]) != ESP_OK ||
        g_frame_buffers[0] == nullptr || g_frame_buffers[1] == nullptr ||
        g_frame_buffers[2] == nullptr)
        return false;
    const ppa_client_config_t ppa_config = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
        .data_burst_length = PPA_DATA_BURST_LENGTH_128,
    };
    if (ppa_register_client(&ppa_config, &g_ppa_client) != ESP_OK) {
        g_ppa_client = nullptr;
        ESP_LOGW(kTag, "PPA direct scanout scale is unavailable");
    }
    const ppa_client_config_t blend_config = {
        .oper_type = PPA_OPERATION_BLEND,
        .max_pending_trans_num = 1,
        .data_burst_length = PPA_DATA_BURST_LENGTH_128,
    };
    if (ppa_register_client(&blend_config, &g_ppa_blend_client) != ESP_OK) {
        g_ppa_blend_client = nullptr;
        ESP_LOGW(kTag, "PPA UI blending is unavailable");
    }
    g_ui_argb = static_cast<lv_color32_t*>(heap_caps_aligned_alloc(
        64, kArgbFrameBytes,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
    if (g_ui_argb == nullptr)
        ESP_LOGW(kTag, "PPA UI blend cache is unavailable");
    else
        std::memset(g_ui_argb, 0, kArgbFrameBytes);
    g_ui_spans = static_cast<pxa_surface_alpha_span_t*>(heap_caps_malloc(
        kMaxUiSpans * sizeof(pxa_surface_alpha_span_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (g_ui_spans == nullptr)
        ESP_LOGW(kTag, "UI span cache is unavailable");
    if (xTaskCreate(PresenterTask, "korvo_pxa", kPresenterStack, nullptr,
                    kPresenterPriority, &g_presenter_task) != pdPASS)
        return false;
    g_display = display;
    g_panel = panel;
    g_frame_done_semaphore = frame_done_semaphore;
    g_transition_callback = transition_callback;
    g_transition_context = transition_context;
    pxa_esp_surface_set_frame_ready_callback(FrameReady, nullptr);
    ESP_LOGI(kTag, "PXA Surface presentation installed (direct + compose)");
    return true;
}

bool DirectScanoutActive() { return g_direct_active; }

void ComposeFrame(uint8_t* pixels) {
    if (pixels == nullptr || g_direct_active) return;
    pxa_esp_surface_frame_t frame;
    if (!pxa_esp_surface_acquire_latest(&frame)) return;
    const int64_t started_us = esp_timer_get_time();
    bool used_ppa = false;
    bool ui_used_ppa = false;
    uint32_t ui_pixels = 0;
    uint32_t ui_duration_us = 0;
    if (frame.format == PXA_SURFACE_FORMAT_RGB565 && frame.visible) {
        used_ppa = TryPpaScaleToBuffer(frame, pixels);
        if (!used_ppa) ComposeRgb565(frame, pixels);
        const int64_t ui_started_us = esp_timer_get_time();
        ui_used_ppa = TryPpaBlendUi(frame, pixels);
        if (!ui_used_ppa)
            ui_pixels = BlendUiAlphaPlane(frame, pixels);
        else
            ui_pixels = g_ui_visible_pixels;
        ui_duration_us = static_cast<uint32_t>(
            esp_timer_get_time() - ui_started_us);
    }
    const uint32_t duration_us =
        static_cast<uint32_t>(esp_timer_get_time() - started_us);
    if (frame.format == PXA_SURFACE_FORMAT_RGB565 && frame.visible) {
        const int64_t now_us = esp_timer_get_time();
        if (g_compose_report_started_us == 0)
            g_compose_report_started_us = now_us;
        ++g_compose_frames;
        if (used_ppa) ++g_compose_ppa_frames;
        if (ui_used_ppa) ++g_compose_ui_ppa_frames;
        g_compose_total_us += duration_us;
        g_compose_max_us = std::max(g_compose_max_us, duration_us);
        g_compose_ui_total_us += ui_duration_us;
        g_compose_ui_max_us = std::max(g_compose_ui_max_us, ui_duration_us);
        g_compose_ui_pixels += ui_pixels;
        if (now_us - g_compose_report_started_us >= 1000 * 1000) {
            const SurfaceGeometry geometry = ResolveGeometry(
                frame.width, frame.height, frame.x, frame.y);
            ESP_LOGI(kTag,
                     "PERF compose=%u ppa=%u ui_ppa=%u time=%u/%u ms "
                     "ui=%u/%u ms pixels=%u spans=%u "
                     "surface=%ux%u scale=%u "
                     "opaque=%u alpha=%u(%ux%u)",
                     g_compose_frames, g_compose_ppa_frames,
                     g_compose_ui_ppa_frames,
                     static_cast<uint32_t>(
                         g_compose_total_us / g_compose_frames) / 1000,
                     g_compose_max_us / 1000,
                     static_cast<uint32_t>(
                         g_compose_ui_total_us / g_compose_frames) / 1000,
                     g_compose_ui_max_us / 1000,
                     static_cast<uint32_t>(
                         g_compose_ui_pixels / g_compose_frames),
                     static_cast<unsigned>(g_ui_span_count),
                     frame.width, frame.height,
                     geometry.scale, frame.opaque_ui_region_count,
                     frame.ui_alpha_plane.visible,
                     frame.ui_alpha_plane.width, frame.ui_alpha_plane.height);
            g_compose_report_started_us = now_us;
            g_compose_frames = 0;
            g_compose_ppa_frames = 0;
            g_compose_ui_ppa_frames = 0;
            g_compose_total_us = 0;
            g_compose_max_us = 0;
            g_compose_ui_total_us = 0;
            g_compose_ui_max_us = 0;
            g_compose_ui_pixels = 0;
        }
    }
    pxa_esp_surface_note_frame_presented(
        frame.input_timestamp_us, static_cast<uint64_t>(esp_timer_get_time()));
    pxa_esp_surface_release_frame(frame.lease);
}

}  // namespace korvo_pxa_surface
