#include "esp_mosaico_pxa_surface.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <driver/ppa.h>
#include <esp_lv_adapter.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <pxa/pxa_esp_surface.h>
#include <pxa/pxa_surface_transform.h>

#include "esp_mosaico_config.h"

namespace mosaico_pxa_surface {
namespace {

lv_display_t* g_display = nullptr;
TaskHandle_t g_presenter = nullptr;
TaskHandle_t g_raster = nullptr;
std::atomic<uint32_t> g_raster_us{0};
DirectPresenter g_direct_presenter = nullptr;
void* g_direct_context = nullptr;
lv_area_t g_last_present_area = {};
bool g_has_last_present_area = false;
ppa_client_handle_t g_ppa_srm = nullptr;
ppa_client_handle_t g_ppa_fill = nullptr;
std::atomic<uint32_t> g_ppa_fills{0};

bool FillRasterBands(void*, uint16_t* pixels, uint32_t stride_pixels,
                     uint16_t width, uint16_t height, const uint16_t* tops,
                     const uint16_t* bottoms, const uint16_t* colors, uint8_t count) {
    if (!g_ppa_fill || !pixels || !tops || !bottoms || !colors || !count ||
        stride_pixels != width || !width || !height) return false;
    // Validate the entire request before the first DMA changes the target.
    for (uint8_t i = 0; i < count; ++i)
        if (tops[i] >= bottoms[i] || bottoms[i] > height) return false;
    for (uint8_t i = 0; i < count; ++i) {
        const uint16_t color = colors[i];
        color_pixel_argb8888_data_t argb = {};
        argb.a = 255;
        const uint8_t r = color >> 11, g = (color >> 5) & 63, b = color & 31;
        argb.r = (r << 3) | (r >> 2);
        argb.g = (g << 2) | (g >> 4);
        argb.b = (b << 3) | (b >> 2);
        const ppa_fill_oper_config_t config = {
            .out = {.buffer = pixels,
                    .buffer_size = static_cast<size_t>(width) * height * sizeof(*pixels),
                    .pic_w = width, .pic_h = height,
                    .block_offset_x = 0, .block_offset_y = tops[i],
                    .fill_cm = PPA_FILL_COLOR_MODE_RGB565},
            .fill_block_w = width,
            .fill_block_h = static_cast<uint32_t>(bottoms[i] - tops[i]),
            .fill_argb_color = argb,
            .mode = PPA_TRANS_MODE_BLOCKING,
        };
        if (ppa_do_fill(g_ppa_fill, &config) != ESP_OK) return false;
    }
    g_ppa_fills.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void InitializeAcceleration() {
    const ppa_client_config_t srm = {
        .oper_type = PPA_OPERATION_SRM, .max_pending_trans_num = 1,
        .data_burst_length = PPA_DATA_BURST_LENGTH_128,
    };
    if (ppa_register_client(&srm, &g_ppa_srm) != ESP_OK) g_ppa_srm = nullptr;
    const ppa_client_config_t fill = {
        .oper_type = PPA_OPERATION_FILL, .max_pending_trans_num = 1,
        .data_burst_length = PPA_DATA_BURST_LENGTH_128,
    };
    if (ppa_register_client(&fill, &g_ppa_fill) != ESP_OK) g_ppa_fill = nullptr;
    if (g_ppa_fill) pxa_esp_surface_set_fill_bands_callback(FillRasterBands, nullptr);
    ESP_LOGI("mosaico_surface", "Raster cores=1+0 outside UI lock; presenter core=1 priority=5; GameRender PPA copy/scale/swap=%d background=%d",
             g_ppa_srm != nullptr, g_ppa_fill != nullptr);
}

bool ResolvePresentArea(lv_area_t* area) {
    pxa_esp_surface_present_info_t info;
    if (!pxa_esp_surface_get_present_info(&info) || !info.visible) return false;
    uint8_t scale = pxa_surface_fit_scale(info.width, info.height,
        mosaico_board::kWidth, mosaico_board::kHeight);
    if (scale == 0) scale = 1;
    const int32_t width = info.width * scale;
    const int32_t height = info.height * scale;
    int32_t x = info.x, y = info.y;
    if (x == 0 && y == 0 &&
        (width < mosaico_board::kWidth || height < mosaico_board::kHeight)) {
        x = (mosaico_board::kWidth - width) / 2;
        y = (mosaico_board::kHeight - height) / 2;
    }
    *area = {std::max<int32_t>(0, x), std::max<int32_t>(0, y),
        std::min<int32_t>(mosaico_board::kWidth, x + width) - 1,
        std::min<int32_t>(mosaico_board::kHeight, y + height) - 1};
    return mosaico_board::ValidDisplayArea(*area);
}

void InvalidateSurface() {
    lv_obj_t* screen = lv_display_get_screen_active(g_display);
    lv_area_t area;
    // Trusted/alpha composition can extend beyond the Surface bounds.
    if (!pxa_esp_surface_composition_required() && ResolvePresentArea(&area)) {
        if (g_has_last_present_area &&
            (area.x1 != g_last_present_area.x1 || area.y1 != g_last_present_area.y1 ||
             area.x2 != g_last_present_area.x2 || area.y2 != g_last_present_area.y2))
            lv_obj_invalidate_area(screen, &g_last_present_area);
        lv_obj_invalidate_area(screen, &area);
        g_last_present_area = area;
        g_has_last_present_area = true;
    } else {
        lv_obj_invalidate(screen);
        g_has_last_present_area = false;
    }
}

void FrameReady(void*) {
    if (g_raster != nullptr) xTaskNotifyGive(g_raster);
}

void Raster(void*) {
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        const int64_t started = esp_timer_get_time();
        if (pxa_esp_surface_prepare_latest_raster())
            g_raster_us.store(esp_timer_get_time() - started, std::memory_order_relaxed);
        // Ordinary Surface frames need no rasterization. Coalesce both kinds
        // into the same presenter notification, retaining only newest pixels.
        if (g_presenter != nullptr)
            xTaskNotifyGive(g_presenter);
    }
}

void Presenter(void*) {
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        // Startup builds the reference UI under this lock. Wait for the owner
        // instead of reporting an expected timeout every 100 ms. Notifications
        // coalesce while blocked, so the newest surface is presented next.
        while (esp_lv_adapter_lock(-1) != ESP_OK) vTaskDelay(1);
        lv_lock();
        const bool presented = g_direct_presenter != nullptr &&
                               g_direct_presenter(g_direct_context);
        if (presented) {
            g_has_last_present_area = ResolvePresentArea(&g_last_present_area);
        } else {
            InvalidateSurface();
        }
        lv_unlock();
        esp_lv_adapter_unlock();
        // Releasing the lease frees a buffer for double-buffered applications.
        if (presented && pxa_esp_surface_has_pending_raster_draw()) FrameReady(nullptr);
    }
}

uint16_t BlendArgb(uint16_t destination, uint32_t argb) {
    const uint8_t alpha = static_cast<uint8_t>(argb >> 24);
    if (alpha == 0) return destination;
    const uint16_t source = static_cast<uint16_t>(
        ((argb >> 8) & 0xf800u) | ((argb >> 5) & 0x07e0u) |
        ((argb >> 3) & 0x001fu));
    if (alpha == 255) return source;
    const uint32_t inverse = 255u - alpha;
    const uint32_t red = (source >> 11) +
        (((destination >> 11) * inverse + 128u) >> 8);
    const uint32_t green = ((source >> 5) & 0x3fu) +
        ((((destination >> 5) & 0x3fu) * inverse + 128u) >> 8);
    const uint32_t blue = (source & 0x1fu) +
        (((destination & 0x1fu) * inverse + 128u) >> 8);
    return static_cast<uint16_t>((red << 11) | (green << 5) | blue);
}

bool OpaqueUiAt(const pxa_esp_surface_frame_t& frame, int32_t surface_x,
                int32_t surface_y) {
    for (uint8_t index = 0; index < frame.opaque_ui_region_count; ++index) {
        const auto& rect = frame.opaque_ui_regions[index];
        if (surface_x >= rect.x && surface_y >= rect.y &&
            surface_x < rect.x + rect.width &&
            surface_y < rect.y + rect.height)
            return true;
    }
    return false;
}

uint16_t ComposePlane(uint16_t background,
                       const pxa_esp_surface_ui_alpha_plane_t& plane,
                       int32_t pixel_x, int32_t pixel_y) {
    if (!plane.visible || plane.opacity == 0 || plane.pixels == nullptr ||
        plane.alpha == nullptr || pixel_x < plane.x || pixel_y < plane.y ||
        pixel_x >= plane.x + plane.width || pixel_y >= plane.y + plane.height)
        return background;
    const size_t offset_x = pixel_x - plane.x;
    const size_t offset_y = pixel_y - plane.y;
    const auto* colors = reinterpret_cast<const uint16_t*>(
        reinterpret_cast<const uint8_t*>(plane.pixels) +
        offset_y * plane.pixel_stride_bytes);
    const uint8_t* alpha = plane.alpha + offset_y * plane.alpha_stride_bytes;
    return pxa_esp_surface_blend_alpha_pixel(
        background, colors[offset_x], alpha[offset_x], plane.opacity);
}

void ComposePlaneArea(const pxa_esp_surface_ui_alpha_plane_t& plane,
                      const lv_area_t& area, uint8_t* pixels, uint32_t stride) {
    if (!plane.visible || !plane.opacity || !plane.pixels || !plane.alpha) return;
    const int32_t left = std::max<int32_t>(area.x1, plane.x);
    const int32_t top = std::max<int32_t>(area.y1, plane.y);
    const int32_t right = std::min<int32_t>(area.x2 + 1, plane.x + plane.width);
    const int32_t bottom = std::min<int32_t>(area.y2 + 1, plane.y + plane.height);
    for (int32_t y = top; y < bottom && left < right; ++y) {
        auto* row = reinterpret_cast<uint16_t*>(pixels + (y - area.y1) * stride);
        const auto* colors = reinterpret_cast<const uint16_t*>(
            reinterpret_cast<const uint8_t*>(plane.pixels) +
            (y - plane.y) * plane.pixel_stride_bytes);
        const uint8_t* alpha = plane.alpha + (y - plane.y) * plane.alpha_stride_bytes;
        for (int32_t x = left; x < right; ++x) {
            const uint8_t a = alpha[x - plane.x];
            if (a == 0) continue;
            const uint16_t source = colors[x - plane.x];
            row[x - area.x1] = __builtin_bswap16(a == 255 && plane.opacity == 255
                ? source : pxa_esp_surface_blend_alpha_pixel(
                    __builtin_bswap16(row[x - area.x1]), source, a, plane.opacity));
        }
    }
}

}

bool Install(lv_display_t* display, DirectPresenter presenter, void* context) {
    if (display == nullptr || g_display != nullptr) return false;
    g_display = display;
    g_direct_presenter = presenter;
    g_direct_context = context;
    // Presenter blocks in PPA/DMA while Raster uses both CPUs for the next
    // frame. Only presentation and LVGL composition own the UI lock.
    if (xTaskCreatePinnedToCore(Raster, "mosaico_raster", 4096, nullptr, 4,
                    &g_raster, 1) != pdPASS) {
        g_display = nullptr;
        return false;
    }
    if (xTaskCreatePinnedToCore(Presenter, "mosaico_pxa", 4096, nullptr, 5,
                    &g_presenter, 1) != pdPASS) {
        vTaskDelete(g_raster);
        g_raster = nullptr;
        g_display = nullptr;
        return false;
    }
    InitializeAcceleration();
    (void)pxa_esp_game_render_set_scale_profile(
        PXA_GAME_RENDER_SCALE_MASK_1X | PXA_GAME_RENDER_SCALE_MASK_2X |
            PXA_GAME_RENDER_SCALE_MASK_4X,
        PXA_GAME_RENDER_SCALE_1X);
    pxa_esp_surface_set_frame_ready_callback(FrameReady, nullptr);
    FrameReady(nullptr);
    return true;
}

bool DirectFrameEligible(const pxa_esp_surface_frame_t& frame) {
    if (!frame.visible || frame.pixels == nullptr || frame.width == 0 ||
        frame.height == 0 || frame.format != PXA_SURFACE_FORMAT_RGB565 ||
        frame.x != 0 || frame.y != 0 || frame.opaque_ui_region_count != 0 ||
        frame.stride_bytes < frame.width * sizeof(uint16_t) ||
        frame.stride_bytes % sizeof(uint16_t) != 0)
        return false;
    // Visible planes must be complete. A missing trusted snapshot deliberately
    // falls back to LVGL rather than exposing the app above host controls.
    for (const auto* plane : {&frame.ui_alpha_plane, &frame.system_alpha_plane}) {
        if (plane == &frame.ui_alpha_plane && frame.suppress_guest_alpha) continue;
        if (plane->visible && (!plane->pixels || !plane->alpha ||
            !plane->width || !plane->height ||
            plane->pixel_stride_bytes < plane->width * sizeof(uint16_t) ||
            plane->pixel_stride_bytes % sizeof(uint16_t) != 0 ||
            plane->alpha_stride_bytes < plane->width)) return false;
    }
    const uint8_t scale = pxa_surface_fit_scale(
        frame.width, frame.height, mosaico_board::kWidth, mosaico_board::kHeight);
    return scale != 0 && frame.width * scale == mosaico_board::kWidth &&
        frame.height * scale == mosaico_board::kHeight;
}

bool TryAcceleratedDirectFrame(const pxa_esp_surface_frame_t& frame, uint8_t* pixels) {
    if (!g_ppa_srm || !pixels || !DirectFrameEligible(frame)) return false;
    const uint8_t scale = pxa_surface_fit_scale(frame.width, frame.height,
        mosaico_board::kWidth, mosaico_board::kHeight);
    const ppa_srm_oper_config_t config = {
        .in = {.buffer = const_cast<uint8_t*>(frame.pixels),
               .pic_w = frame.stride_bytes / 2u, .pic_h = frame.height,
               .block_w = frame.width, .block_h = frame.height,
               .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
        .out = {.buffer = pixels,
                .buffer_size = static_cast<size_t>(mosaico_board::kWidth) * mosaico_board::kHeight * 2,
                .pic_w = mosaico_board::kWidth, .pic_h = mosaico_board::kHeight,
                .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = static_cast<float>(scale), .scale_y = static_cast<float>(scale),
        // CO5300 needs big-endian RGB565. PPA's input byte swap preserves
        // those bytes through RGB565 -> RGB565, including exact 2x/4x scaling.
        .byte_swap = true,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    if (ppa_do_scale_rotate_mirror(g_ppa_srm, &config) != ESP_OK) return false;
    const lv_area_t full = {0, 0, mosaico_board::kWidth - 1, mosaico_board::kHeight - 1};
    if (!frame.suppress_guest_alpha)
        ComposePlaneArea(frame.ui_alpha_plane, full, pixels, mosaico_board::kWidth * 2);
    ComposePlaneArea(frame.system_alpha_plane, full, pixels, mosaico_board::kWidth * 2);
    return true;
}

uint32_t RasterTimeUs() { return g_raster_us.load(std::memory_order_relaxed); }

uint32_t AcceleratedFillCount() { return g_ppa_fills.load(std::memory_order_relaxed); }

uint64_t ComposeFrame(const pxa_esp_surface_frame_t& frame,
                      const lv_area_t* area, uint8_t* pixels,
                      uint32_t stride_bytes) {
    if (area == nullptr || pixels == nullptr ||
        !mosaico_board::ValidDisplayArea(*area))
        return 0;
    const size_t area_width = lv_area_get_width(area);
    if (stride_bytes == 0) stride_bytes = area_width * sizeof(uint16_t);
    if (stride_bytes < area_width * sizeof(uint16_t) || stride_bytes % 2 != 0)
        return 0;
    const size_t bytes_per_pixel =
        frame.format == PXA_SURFACE_FORMAT_RGB565 ? 2 : 4;
    if (frame.pixels == nullptr || frame.width == 0 || frame.height == 0 ||
        (frame.format != PXA_SURFACE_FORMAT_RGB565 &&
         frame.format != PXA_SURFACE_FORMAT_ARGB8888_PREMULTIPLIED) ||
        frame.stride_bytes < frame.width * bytes_per_pixel)
        return 0;
    uint8_t scale = pxa_surface_fit_scale(
        frame.width, frame.height, mosaico_board::kWidth, mosaico_board::kHeight);
    if (scale == 0) scale = 1;
    int32_t origin_x = frame.x;
    int32_t origin_y = frame.y;
    const int32_t drawn_width = frame.width * scale;
    const int32_t drawn_height = frame.height * scale;
    if (origin_x == 0 && origin_y == 0 &&
        (drawn_width < mosaico_board::kWidth ||
         drawn_height < mosaico_board::kHeight)) {
        origin_x = (mosaico_board::kWidth - drawn_width) / 2;
        origin_y = (mosaico_board::kHeight - drawn_height) / 2;
    }
    const bool plain_rgb565 = frame.format == PXA_SURFACE_FORMAT_RGB565 &&
        frame.opaque_ui_region_count == 0;
    if (plain_rgb565) {
        const int32_t left = std::max<int32_t>(area->x1, origin_x);
        const int32_t right = std::min<int32_t>(area->x2 + 1, origin_x + drawn_width);
        const int32_t top = std::max<int32_t>(area->y1, origin_y);
        const int32_t bottom = std::min<int32_t>(area->y2 + 1, origin_y + drawn_height);
        for (int32_t pixel_y = top; pixel_y < bottom && left < right; ++pixel_y) {
            auto* row = reinterpret_cast<uint16_t*>(
                pixels + (pixel_y - area->y1) * stride_bytes);
            if (pixel_y > top && (pixel_y - origin_y) % scale != 0) {
                // Repeated rows have already been expanded and byte-swapped.
                std::memcpy(row + left - area->x1,
                    pixels + (pixel_y - area->y1 - 1) * stride_bytes +
                        (left - area->x1) * sizeof(uint16_t),
                    (right - left) * sizeof(uint16_t));
                continue;
            }
            const uint8_t* source = frame.pixels +
                (pixel_y - origin_y) / scale * frame.stride_bytes;
            int32_t pixel_x = left;
            if (scale == 1) {
                for (; pixel_x + 1 < right; pixel_x += 2) {
                    uint32_t pair;
                    std::memcpy(&pair, source + (pixel_x - origin_x) * 2, 4);
                    pair = ((pair & 0x00ff00ffu) << 8) |
                           ((pair & 0xff00ff00u) >> 8);
                    std::memcpy(row + pixel_x - area->x1, &pair, 4);
                }
            }
            for (; pixel_x < right;) {
                const int32_t relative_x = pixel_x - origin_x;
                uint16_t value;
                std::memcpy(&value, source + relative_x / scale * sizeof(value),
                            sizeof(value));
                value = __builtin_bswap16(value);
                int32_t repeat = std::min<int32_t>(
                    scale - relative_x % scale, right - pixel_x);
                const uint32_t pair = static_cast<uint32_t>(value) * 0x00010001u;
                while (repeat >= 2) {
                    std::memcpy(row + pixel_x - area->x1, &pair, 4);
                    pixel_x += 2;
                    repeat -= 2;
                }
                if (repeat != 0) row[pixel_x++ - area->x1] = value;
            }
        }
        // Copy/scale the game with the fast RGB565 path, then blend only
        // overlay intersections. Hidden or transparent pixels do no blending.
        if (!frame.suppress_guest_alpha)
            ComposePlaneArea(frame.ui_alpha_plane, *area, pixels, stride_bytes);
        ComposePlaneArea(frame.system_alpha_plane, *area, pixels, stride_bytes);
        return frame.input_timestamp_us;
    }
    for (int32_t pixel_y = area->y1; pixel_y <= area->y2; ++pixel_y) {
        auto* row = reinterpret_cast<uint16_t*>(
            pixels + (pixel_y - area->y1) * stride_bytes);
        for (int32_t pixel_x = area->x1; pixel_x <= area->x2; ++pixel_x) {
            uint16_t value = __builtin_bswap16(row[pixel_x - area->x1]);
            if (pixel_x >= origin_x && pixel_y >= origin_y &&
                pixel_x < origin_x + drawn_width &&
                pixel_y < origin_y + drawn_height) {
                const int32_t surface_x = (pixel_x - origin_x) / scale;
                const int32_t surface_y = (pixel_y - origin_y) / scale;
                if (!OpaqueUiAt(frame, surface_x, surface_y)) {
                    const uint8_t* source = frame.pixels +
                        surface_y * frame.stride_bytes +
                        surface_x * bytes_per_pixel;
                    if (frame.format == PXA_SURFACE_FORMAT_RGB565) {
                        value = static_cast<uint16_t>(source[0] | source[1] << 8);
                    } else {
                        uint32_t argb;
                        std::memcpy(&argb, source, sizeof(argb));
                        value = BlendArgb(value, argb);
                    }
                }
            }
            if (!frame.suppress_guest_alpha)
                value = ComposePlane(value, frame.ui_alpha_plane,
                                      pixel_x, pixel_y);
            value = ComposePlane(value, frame.system_alpha_plane,
                                  pixel_x, pixel_y);
            row[pixel_x - area->x1] = __builtin_bswap16(value);
        }
    }
    return frame.input_timestamp_us;
}

uint64_t ComposeFlushArea(const lv_area_t* area, uint8_t* pixels,
                          uint32_t stride_bytes) {
    if (area == nullptr || pixels == nullptr ||
        !mosaico_board::ValidDisplayArea(*area)) return 0;
    const size_t row_bytes = lv_area_get_width(area) * sizeof(uint16_t);
    if (stride_bytes != 0 && (stride_bytes < row_bytes || stride_bytes % 2 != 0))
        return 0;
    pxa_esp_surface_frame_t frame;
    if (!pxa_esp_surface_acquire_prepared(&frame, false)) return 0;
    const uint64_t timestamp = ComposeFrame(frame, area, pixels, stride_bytes);
    pxa_esp_surface_release_frame(frame.lease);
    if (pxa_esp_surface_has_pending_raster_draw()) FrameReady(nullptr);
    return timestamp;
}

}
