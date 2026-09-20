#include "esp32s31_korvo_1_pxa_surface.h"

#include <algorithm>
#include <cstring>

#include <driver/ppa.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_rgb.h>
#include <esp_lvgl_port.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <pxa/pxa_esp_surface.h>
#include <pxa/pxa_surface_transform.h>

#include "esp32s31_korvo_1_config.h"

namespace korvo_pxa_surface {
namespace {

constexpr char kTag[] = "KorvoPxa";
constexpr uint32_t kPresenterStack = 12288;
constexpr UBaseType_t kPresenterPriority = 3;
constexpr int32_t kLockTimeoutMs = 100;
constexpr uint32_t kVsyncTimeoutMs = 100;
constexpr size_t kFrameBytes =
    KORVO_DISPLAY_WIDTH * KORVO_DISPLAY_HEIGHT * sizeof(uint16_t);

lv_display_t* g_display = nullptr;
TaskHandle_t g_presenter_task = nullptr;
esp_lcd_panel_handle_t g_panel = nullptr;
SemaphoreHandle_t g_vsync_semaphore = nullptr;
ppa_client_handle_t g_ppa_client = nullptr;
void* g_frame_buffers[2] = {};
uint8_t g_direct_buffer_index = 0;
bool g_direct_active = false;
uint64_t g_last_direct_frame_id = 0;
lv_area_t g_last_present_area = {};
bool g_has_last_present_area = false;

int64_t g_compose_report_started_us = 0;
uint32_t g_compose_frames = 0;
uint32_t g_compose_ppa_frames = 0;
uint64_t g_compose_total_us = 0;
uint32_t g_compose_max_us = 0;

int64_t g_direct_report_started_us = 0;
uint32_t g_direct_frames = 0;
uint64_t g_direct_compose_total_us = 0;
uint32_t g_direct_compose_max_us = 0;
uint64_t g_direct_vsync_total_us = 0;
uint32_t g_direct_vsync_max_us = 0;
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
        frame.opaque_ui_region_count != 0 ||
        (frame.ui_alpha_plane.visible && frame.ui_alpha_plane.opacity != 0))
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

void ComposeFullFrame(const pxa_esp_surface_frame_t& frame, uint8_t* target) {
    if (TryPpaScaleToBuffer(frame, target)) return;
    ComposeRgb565(frame, target);
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

void PresentDirectFrame(const pxa_esp_surface_frame_t& frame) {
    if (g_panel == nullptr || g_frame_buffers[0] == nullptr ||
        g_frame_buffers[1] == nullptr)
        return;
    uint8_t* target =
        static_cast<uint8_t*>(g_frame_buffers[g_direct_buffer_index]);
    const int64_t compose_started_us = esp_timer_get_time();
    ComposeFullFrame(frame, target);
    const int64_t compose_finished_us = esp_timer_get_time();
    if (esp_lcd_panel_draw_bitmap(g_panel, 0, 0, KORVO_DISPLAY_WIDTH,
                                  KORVO_DISPLAY_HEIGHT,
                                  target) != ESP_OK) {
        ESP_LOGW(kTag, "Direct scanout buffer switch failed");
        return;
    }
    (void)xSemaphoreTake(g_vsync_semaphore, 0);
    const bool vsync_ready =
        xSemaphoreTake(g_vsync_semaphore,
                       pdMS_TO_TICKS(kVsyncTimeoutMs)) == pdTRUE;
    const int64_t finished_us = esp_timer_get_time();
    if (!vsync_ready) ++g_direct_timeouts;
    g_direct_buffer_index ^= 1U;
    pxa_esp_surface_note_frame_presented(
        frame.input_timestamp_us, static_cast<uint64_t>(finished_us));

    const uint32_t compose_us =
        static_cast<uint32_t>(compose_finished_us - compose_started_us);
    const uint32_t vsync_us =
        static_cast<uint32_t>(finished_us - compose_finished_us);
    ++g_direct_frames;
    g_direct_compose_total_us += compose_us;
    g_direct_compose_max_us = std::max(g_direct_compose_max_us, compose_us);
    g_direct_vsync_total_us += vsync_us;
    g_direct_vsync_max_us = std::max(g_direct_vsync_max_us, vsync_us);
    const int64_t now_us = esp_timer_get_time();
    if (g_direct_report_started_us == 0) g_direct_report_started_us = now_us;
    if (now_us - g_direct_report_started_us >= 1000 * 1000) {
        ESP_LOGI(kTag,
                 "PERF direct=%u compose=%u/%u ms vsync=%u/%u ms timeout=%u",
                 g_direct_frames,
                 static_cast<uint32_t>(
                     g_direct_compose_total_us / g_direct_frames) / 1000,
                 g_direct_compose_max_us / 1000,
                 static_cast<uint32_t>(
                     g_direct_vsync_total_us / g_direct_frames) / 1000,
                 g_direct_vsync_max_us / 1000, g_direct_timeouts);
        g_direct_report_started_us = now_us;
        g_direct_frames = 0;
        g_direct_compose_total_us = 0;
        g_direct_compose_max_us = 0;
        g_direct_vsync_total_us = 0;
        g_direct_vsync_max_us = 0;
        g_direct_timeouts = 0;
    }
}

bool EnterDirectScanout() {
    if (g_direct_active) return true;
    if (!lvgl_port_lock(kLockTimeoutMs)) return false;
    SetRefreshPaused(true);
    g_direct_active = true;
    g_direct_buffer_index = 0;
    g_last_direct_frame_id = 0;
    lvgl_port_unlock();
    ESP_LOGI(kTag, "Direct scanout engaged");
    return true;
}

void ExitDirectScanout() {
    if (!g_direct_active) return;
    if (!lvgl_port_lock(kLockTimeoutMs)) return;
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
                g_last_direct_frame_id = frame.frame_id;
                PresentDirectFrame(frame);
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
                    g_last_direct_frame_id = frame.frame_id;
                    PresentDirectFrame(frame);
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
             SemaphoreHandle_t vsync_semaphore) {
    if (display == nullptr || panel == nullptr || vsync_semaphore == nullptr ||
        g_display != nullptr)
        return false;
    if (esp_lcd_rgb_panel_get_frame_buffer(panel, 2, &g_frame_buffers[0],
                                           &g_frame_buffers[1]) != ESP_OK ||
        g_frame_buffers[0] == nullptr || g_frame_buffers[1] == nullptr)
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
    if (xTaskCreate(PresenterTask, "korvo_pxa", kPresenterStack, nullptr,
                    kPresenterPriority, &g_presenter_task) != pdPASS)
        return false;
    g_display = display;
    g_panel = panel;
    g_vsync_semaphore = vsync_semaphore;
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
    if (frame.format == PXA_SURFACE_FORMAT_RGB565 && frame.visible) {
        used_ppa = TryPpaScaleToBuffer(frame, pixels);
        if (!used_ppa) ComposeRgb565(frame, pixels);
    }
    const uint32_t duration_us =
        static_cast<uint32_t>(esp_timer_get_time() - started_us);
    if (frame.format == PXA_SURFACE_FORMAT_RGB565 && frame.visible) {
        const int64_t now_us = esp_timer_get_time();
        if (g_compose_report_started_us == 0)
            g_compose_report_started_us = now_us;
        ++g_compose_frames;
        if (used_ppa) ++g_compose_ppa_frames;
        g_compose_total_us += duration_us;
        g_compose_max_us = std::max(g_compose_max_us, duration_us);
        if (now_us - g_compose_report_started_us >= 1000 * 1000) {
            const SurfaceGeometry geometry = ResolveGeometry(
                frame.width, frame.height, frame.x, frame.y);
            ESP_LOGI(kTag,
                     "PERF compose=%u ppa=%u time=%u/%u ms "
                     "surface=%ux%u scale=%u "
                     "opaque=%u alpha=%u(%ux%u)",
                     g_compose_frames, g_compose_ppa_frames,
                     static_cast<uint32_t>(
                         g_compose_total_us / g_compose_frames) / 1000,
                     g_compose_max_us / 1000, frame.width, frame.height,
                     geometry.scale, frame.opaque_ui_region_count,
                     frame.ui_alpha_plane.visible,
                     frame.ui_alpha_plane.width, frame.ui_alpha_plane.height);
            g_compose_report_started_us = now_us;
            g_compose_frames = 0;
            g_compose_ppa_frames = 0;
            g_compose_total_us = 0;
            g_compose_max_us = 0;
        }
    }
    pxa_esp_surface_note_frame_presented(
        frame.input_timestamp_us, static_cast<uint64_t>(esp_timer_get_time()));
    pxa_esp_surface_release_frame(frame.lease);
}

}  // namespace korvo_pxa_surface
