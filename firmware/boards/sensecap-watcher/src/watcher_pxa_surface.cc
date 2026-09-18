#include "watcher_pxa_surface.h"

#include <algorithm>
#include <cstring>

#include <esp_lvgl_port.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <pxa/pxa_esp_surface.h>
#include <pxa/pxa_surface_transform.h>

#include "sensecap_watcher_config.h"

namespace watcher_pxa_surface {
namespace {

constexpr char kTag[] = "WatcherPxa";
// The task only takes the LVGL lock and invalidates the screen.
constexpr uint32_t kPresenterTaskStack = 3072;
constexpr UBaseType_t kPresenterTaskPriority = 3;
constexpr uint32_t kPresenterLockTimeoutMs = 100;

lv_display_t* g_display = nullptr;
TaskHandle_t g_presenter_task = nullptr;
lv_area_t g_last_present_area = {};
bool g_has_last_present_area = false;

uint16_t SwapBytes(uint16_t value) { return __builtin_bswap16(value); }

// Mirrors the pai-touch tile worker: the trusted alpha overlay carries
// premultiplied RGB565 plus an A8 coverage plane.
uint16_t BlendSerializedAlpha(uint16_t destination, uint32_t argb) {
    const uint8_t alpha = static_cast<uint8_t>(argb >> 24);
    if (alpha == 0) return destination;
    const uint16_t source = static_cast<uint16_t>(
        ((argb >> 8) & 0xf800u) | ((argb >> 5) & 0x07e0u) |
        ((argb >> 3) & 0x001fu));
    if (alpha == 255) return source;
    // Use independent 16-bit lanes for red and blue before multiplying:
    // red's discarded fractional bits must not leak into blue.
    const uint32_t inverse = 255u - alpha;
    const uint32_t red_blue_lanes = (destination >> 11) |
        (static_cast<uint32_t>(destination & 0x001fu) << 16);
    const uint32_t retained_red_blue_lanes = red_blue_lanes * inverse;
    const uint16_t retained_red = static_cast<uint16_t>(
        ((retained_red_blue_lanes + 128u) >> 8) & 0x001fu);
    const uint16_t retained_blue = static_cast<uint16_t>(
        ((retained_red_blue_lanes >> 16) + 128u) >> 8) & 0x001fu;
    const uint16_t retained_green = static_cast<uint16_t>(
        ((static_cast<uint32_t>(destination & 0x07e0u) * inverse +
          (128u << 5)) >> 8) >> 5);
    return static_cast<uint16_t>(
        (((source >> 11) + retained_red) << 11) |
        ((((source >> 5) & 0x003fu) + retained_green) << 5) |
        ((source & 0x001fu) + retained_blue));
}

uint16_t BlendUiAlphaPixel(uint16_t destination, uint16_t foreground,
                           uint8_t alpha, uint8_t opacity) {
    if (alpha == 0 || opacity == 0) return destination;
    if (opacity != 255) {
        alpha = static_cast<uint8_t>(
            (static_cast<uint16_t>(alpha) * opacity + 127u) / 255u);
        if (alpha == 0) return destination;
    }
    if (alpha == 255) return foreground;
    const uint16_t inverse = static_cast<uint16_t>(255u - alpha);
    const uint16_t red = static_cast<uint16_t>(
        (((foreground >> 11) * alpha + (destination >> 11) * inverse +
          128u) >> 8));
    const uint16_t green = static_cast<uint16_t>(
        ((((foreground >> 5) & 0x3fu) * alpha +
          ((destination >> 5) & 0x3fu) * inverse + 128u) >> 8));
    const uint16_t blue = static_cast<uint16_t>(
        (((foreground & 0x1fu) * alpha + (destination & 0x1fu) * inverse +
          128u) >> 8));
    return static_cast<uint16_t>((red << 11) | (green << 5) | blue);
}

// A Surface smaller than the panel is presented at the largest exact 1x/2x/4x
// factor that fits and is centered unless the Guest placed it explicitly; the
// panel's safe area is the inscribed square of the round display, so a
// top-left 296x240 Surface would fall outside it. The Guest repeats this fit
// computation to map panel input coordinates back into Surface pixels.
void ResolveSurfaceGeometry(uint16_t frame_width, uint16_t frame_height,
                            int32_t frame_x, int32_t frame_y,
                            int32_t* origin_x, int32_t* origin_y,
                            int32_t* drawn_width, int32_t* drawn_height,
                            uint8_t* scale) {
    uint8_t factor = pxa_surface_fit_scale(
        frame_width, frame_height,
        static_cast<uint16_t>(WATCHER_DISPLAY_WIDTH),
        static_cast<uint16_t>(WATCHER_DISPLAY_HEIGHT));
    if (factor == 0) factor = 1;
    int32_t x = frame_x;
    int32_t y = frame_y;
    const int32_t width = static_cast<int32_t>(frame_width) * factor;
    const int32_t height = static_cast<int32_t>(frame_height) * factor;
    if (frame_x == 0 && frame_y == 0 &&
        (width < WATCHER_DISPLAY_WIDTH || height < WATCHER_DISPLAY_HEIGHT)) {
        x = (WATCHER_DISPLAY_WIDTH - width) / 2;
        y = (WATCHER_DISPLAY_HEIGHT - height) / 2;
    }
    *origin_x = x;
    *origin_y = y;
    *drawn_width = width;
    *drawn_height = height;
    *scale = factor;
}

bool ResolvePresentArea(lv_area_t* area) {
    pxa_esp_surface_present_info_t info;
    int32_t origin_x;
    int32_t origin_y;
    int32_t drawn_width;
    int32_t drawn_height;
    uint8_t scale;
    if (area == nullptr || !pxa_esp_surface_get_present_info(&info) ||
        !info.visible)
        return false;
    ResolveSurfaceGeometry(info.width, info.height, info.x, info.y, &origin_x,
                           &origin_y, &drawn_width, &drawn_height, &scale);
    (void)scale;
    area->x1 = static_cast<lv_coord_t>(std::max<int32_t>(origin_x, 0));
    area->y1 = static_cast<lv_coord_t>(std::max<int32_t>(origin_y, 0));
    area->x2 = static_cast<lv_coord_t>(std::min<int32_t>(
        origin_x + drawn_width, WATCHER_DISPLAY_WIDTH) - 1);
    area->y2 = static_cast<lv_coord_t>(std::min<int32_t>(
        origin_y + drawn_height, WATCHER_DISPLAY_HEIGHT) - 1);
    return area->x1 <= area->x2 && area->y1 <= area->y2;
}

bool AreasEqual(const lv_area_t& lhs, const lv_area_t& rhs) {
    return lhs.x1 == rhs.x1 && lhs.y1 == rhs.y1 && lhs.x2 == rhs.x2 &&
           lhs.y2 == rhs.y2;
}

void ComposeRgb565Scale1(const pxa_esp_surface_frame_t& frame,
                         const lv_area_t* area, uint8_t* pixels,
                         int32_t origin_x, int32_t origin_y,
                         int32_t drawn_width, int32_t drawn_height) {
    const int32_t area_width = lv_area_get_width(area);
    const int32_t left = std::max<int32_t>(area->x1, origin_x);
    const int32_t right = std::min<int32_t>(
        area->x2 + 1, origin_x + drawn_width);
    const int32_t top = std::max<int32_t>(area->y1, origin_y);
    const int32_t bottom = std::min<int32_t>(
        area->y2 + 1, origin_y + drawn_height);
    if (left >= right || top >= bottom) return;

    for (int32_t y = top; y < bottom; ++y) {
        const uint8_t* source = frame.pixels +
            static_cast<size_t>(y - origin_y) * frame.stride_bytes +
            static_cast<size_t>(left - origin_x) * 2u;
        uint16_t* destination = reinterpret_cast<uint16_t*>(pixels) +
            static_cast<size_t>(y - area->y1) * area_width +
            (left - area->x1);
        int32_t remaining = right - left;
        while (remaining >= 2) {
            uint32_t pair;
            std::memcpy(&pair, source, sizeof(pair));
            pair = ((pair & UINT32_C(0x00ff00ff)) << 8u) |
                   ((pair & UINT32_C(0xff00ff00)) >> 8u);
            std::memcpy(destination, &pair, sizeof(pair));
            source += sizeof(pair);
            destination += 2;
            remaining -= 2;
        }
        if (remaining != 0) {
            uint16_t value;
            std::memcpy(&value, source, sizeof(value));
            *destination = SwapBytes(value);
        }
    }
}

void ComposeRgb565Scale2(const pxa_esp_surface_frame_t& frame,
                         const lv_area_t* area, uint8_t* pixels,
                         int32_t origin_x, int32_t origin_y,
                         int32_t drawn_width, int32_t drawn_height) {
    const int32_t area_width = lv_area_get_width(area);
    const int32_t left = std::max<int32_t>(area->x1, origin_x);
    const int32_t right = std::min<int32_t>(
        area->x2 + 1, origin_x + drawn_width);
    const int32_t top = std::max<int32_t>(area->y1, origin_y);
    const int32_t bottom = std::min<int32_t>(
        area->y2 + 1, origin_y + drawn_height);
    if (left >= right || top >= bottom) return;

    for (int32_t y = top; y < bottom; ++y) {
        const int32_t source_y = (y - origin_y) >> 1;
        const uint8_t* source = frame.pixels +
            static_cast<size_t>(source_y) * frame.stride_bytes;
        uint16_t* destination = reinterpret_cast<uint16_t*>(pixels) +
            static_cast<size_t>(y - area->y1) * area_width +
            (left - area->x1);
        int32_t x = left;
        int32_t source_x = (x - origin_x) >> 1;

        // A partial LVGL flush can begin at the second half of a scaled pixel.
        if (((x - origin_x) & 1) != 0) {
            uint16_t value;
            std::memcpy(&value, source + source_x * 2, sizeof(value));
            *destination++ = SwapBytes(value);
            ++x;
            ++source_x;
        }
        while (x + 1 < right) {
            uint16_t value;
            std::memcpy(&value, source + source_x * 2, sizeof(value));
            const uint32_t doubled = static_cast<uint32_t>(SwapBytes(value)) *
                0x00010001u;
            std::memcpy(destination, &doubled, sizeof(doubled));
            destination += 2;
            x += 2;
            ++source_x;
        }
        if (x < right) {
            uint16_t value;
            std::memcpy(&value, source + source_x * 2, sizeof(value));
            *destination = SwapBytes(value);
        }
    }
}

void ComposeFrame(const pxa_esp_surface_frame_t& frame,
                  const lv_area_t* area, uint8_t* pixels) {
    const int32_t area_width = lv_area_get_width(area);
    const int32_t area_height = lv_area_get_height(area);
    if (area_width <= 0 || area_height <= 0 || frame.pixels == nullptr ||
        !frame.visible)
        return;

    int32_t origin_x;
    int32_t origin_y;
    int32_t drawn_width;
    int32_t drawn_height;
    uint8_t scale;
    ResolveSurfaceGeometry(frame.width, frame.height, frame.x, frame.y,
                           &origin_x, &origin_y, &drawn_width, &drawn_height,
                           &scale);
    const int32_t surface_right = origin_x + drawn_width;
    const int32_t surface_bottom = origin_y + drawn_height;
    const size_t bytes_per_pixel =
        frame.format == PXA_SURFACE_FORMAT_RGB565 ? 2u : 4u;
    const auto& plane = frame.ui_alpha_plane;
    const bool plane_visible = plane.visible && plane.opacity != 0 &&
                               plane.pixels != nullptr && plane.alpha != nullptr;
    const int32_t plane_right = plane.x + plane.width;
    const int32_t plane_bottom = plane.y + plane.height;

    if (frame.format == PXA_SURFACE_FORMAT_RGB565 &&
        frame.opaque_ui_region_count == 0 && !plane_visible) {
        if (scale == 1) {
            ComposeRgb565Scale1(frame, area, pixels, origin_x, origin_y,
                                drawn_width, drawn_height);
            return;
        }
        if (scale == 2) {
            ComposeRgb565Scale2(frame, area, pixels, origin_x, origin_y,
                                drawn_width, drawn_height);
            return;
        }
    }

    for (int32_t y = area->y1; y <= area->y2; ++y) {
        uint16_t* row = reinterpret_cast<uint16_t*>(
            pixels + static_cast<size_t>(y - area->y1) * area_width * 2u);
        const bool row_in_surface = y >= origin_y && y < surface_bottom;
        const int32_t surface_y =
            row_in_surface ? (y - origin_y) / scale : -1;
        const uint8_t* surface_row =
            row_in_surface
                ? frame.pixels +
                      static_cast<size_t>(surface_y) * frame.stride_bytes
                : nullptr;
        const pxa_surface_damage_rect_t*
            opaque_regions[PXA_SURFACE_MAX_OPAQUE_UI_REGIONS];
        uint8_t opaque_region_count = 0;
        if (surface_row != nullptr) {
            for (uint8_t index = 0; index < frame.opaque_ui_region_count;
                 ++index) {
                const auto& rect = frame.opaque_ui_regions[index];
                if (surface_y >= rect.y &&
                    surface_y < static_cast<int32_t>(rect.y) + rect.height)
                    opaque_regions[opaque_region_count++] = &rect;
            }
        }
        const bool row_has_plane =
            plane_visible && y >= plane.y && y < plane_bottom;
        const uint16_t* plane_colors = nullptr;
        const uint8_t* plane_alpha = nullptr;
        if (row_has_plane) {
            const int32_t plane_y = y - plane.y;
            plane_colors = reinterpret_cast<const uint16_t*>(
                reinterpret_cast<const uint8_t*>(plane.pixels) +
                static_cast<size_t>(plane_y) * plane.pixel_stride_bytes);
            plane_alpha = plane.alpha +
                static_cast<size_t>(plane_y) * plane.alpha_stride_bytes;
        }

        for (int32_t x = area->x1; x <= area->x2; ++x) {
            uint16_t value = SwapBytes(row[x - area->x1]);
            if (surface_row != nullptr && x >= origin_x && x < surface_right) {
                const int32_t surface_x = (x - origin_x) / scale;
                bool use_lvgl = false;
                for (uint8_t index = 0; index < opaque_region_count; ++index) {
                    const auto& rect = *opaque_regions[index];
                    if (surface_x >= rect.x &&
                        surface_x <
                            static_cast<int32_t>(rect.x) + rect.width) {
                        use_lvgl = true;
                        break;
                    }
                }
                if (!use_lvgl) {
                    const uint8_t* pixel = surface_row +
                        static_cast<size_t>(surface_x) * bytes_per_pixel;
                    if (frame.format == PXA_SURFACE_FORMAT_RGB565) {
                        value = static_cast<uint16_t>(
                            pixel[0] | (pixel[1] << 8));
                    } else {
                        uint32_t argb;
                        std::memcpy(&argb, pixel, sizeof(argb));
                        value = BlendSerializedAlpha(value, argb);
                    }
                }
            }
            if (row_has_plane && x >= plane.x && x < plane_right) {
                value = BlendUiAlphaPixel(value,
                                          plane_colors[x - plane.x],
                                          plane_alpha[x - plane.x],
                                          plane.opacity);
            }
            row[x - area->x1] = SwapBytes(value);
        }
    }
}

void FrameReady(void*) {
    if (g_presenter_task != nullptr) xTaskNotifyGive(g_presenter_task);
}

void PresenterTaskEntry(void*) {
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (g_display == nullptr) continue;
        if (!lvgl_port_lock(kPresenterLockTimeoutMs)) continue;
        lv_obj_t* screen = lv_display_get_screen_active(g_display);
        if (screen != nullptr) {
            lv_area_t area;
            if (ResolvePresentArea(&area)) {
                if (g_has_last_present_area &&
                    !AreasEqual(g_last_present_area, area)) {
                    (void)lv_obj_invalidate_area(screen,
                                                 &g_last_present_area);
                }
                (void)lv_obj_invalidate_area(screen, &area);
                g_last_present_area = area;
                g_has_last_present_area = true;
            } else {
                lv_obj_invalidate(screen);
                g_has_last_present_area = false;
            }
        }
        lvgl_port_unlock();
    }
}

}  // namespace

bool Install(lv_display_t* display) {
    if (display == nullptr || g_display != nullptr) return false;
    if (xTaskCreate(PresenterTaskEntry, "watcher_pxa", kPresenterTaskStack,
                    nullptr, kPresenterTaskPriority,
                    &g_presenter_task) != pdPASS) {
        g_presenter_task = nullptr;
        return false;
    }
    g_display = display;
    pxa_esp_surface_set_frame_ready_callback(FrameReady, nullptr);
    // A Surface may already exist when the board installs the presenter;
    // repaint once so its first frame is composed even without a new submit.
    if (lvgl_port_lock(kPresenterLockTimeoutMs)) {
        lv_obj_t* screen = lv_display_get_screen_active(display);
        if (screen != nullptr) lv_obj_invalidate(screen);
        lvgl_port_unlock();
    }
    ESP_LOGI(kTag, "PXA Surface presentation installed");
    return true;
}

void ComposeFlushArea(const lv_area_t* area, uint8_t* pixels) {
    if (area == nullptr || pixels == nullptr) return;
    pxa_esp_surface_frame_t frame;
    if (!pxa_esp_surface_acquire_latest(&frame)) return;
    ComposeFrame(frame, area, pixels);
    pxa_esp_surface_note_frame_presented(
        frame.input_timestamp_us, static_cast<uint64_t>(esp_timer_get_time()));
    pxa_esp_surface_release_frame(frame.lease);
}

}  // namespace watcher_pxa_surface
