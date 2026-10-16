#include "pxa_board_api.h"

#include <nvs.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <stdatomic.h>

static const pxa_board_port_t* g_board_port;
static _Atomic uint8_t g_performance_loaded;
static _Atomic uint8_t g_performance_overlay;
static _Atomic uint8_t g_performance_log;
static _Atomic uint8_t g_pxa_debug_enabled = 1;
/* These two flags share one byte, filling the old settings alignment gap. */
enum { SAMPLE_BUSY = 1, SAMPLE_INITIALIZED = 2 };
static _Atomic uint8_t g_performance_sampling;
static uint32_t g_frame_window_started_us;
static _Atomic uint32_t g_frame_window_count;
static _Atomic uint32_t g_fps_x10;

static void load_performance_settings(void) {
    nvs_handle_t handle;
    uint8_t value;
    if (g_performance_loaded) return;
    if (atomic_exchange_explicit(&g_performance_loaded, 1, memory_order_relaxed)) return;
    if (nvs_open("pxa_perf", NVS_READONLY, &handle) != ESP_OK) return;
    if (nvs_get_u8(handle, "overlay", &value) == ESP_OK)
        g_performance_overlay = value != 0;
    if (nvs_get_u8(handle, "log", &value) == ESP_OK)
        g_performance_log = value != 0;
    if (nvs_get_u8(handle, "pxadb", &value) == ESP_OK)
        g_pxa_debug_enabled = value != 0;
    nvs_close(handle);
}

bool pxa_board_register(const pxa_board_port_t* port) {
    if (port == NULL || port->struct_size != sizeof(*port) ||
        port->initialize == NULL || port->display == NULL ||
        port->display_profile == NULL ||
        ((port->lock_display == NULL) != (port->unlock_display == NULL))) {
        return false;
    }
    if (g_board_port != NULL && g_board_port != port)
        return false;
    g_board_port = port;
    return true;
}

const pxa_board_port_t* pxa_board_current(void) {
    return g_board_port;
}

bool pxa_board_performance_get(pxsys_reference_performance_option_t option) {
    load_performance_settings();
    switch (option) {
        case PXSYS_REFERENCE_PERFORMANCE_OVERLAY:
            return g_performance_overlay != 0;
        case PXSYS_REFERENCE_PERFORMANCE_LOG:
            return g_performance_log != 0;
        case PXSYS_REFERENCE_PXADB:
            return g_pxa_debug_enabled != 0;
    }
    return false;
}

bool pxa_board_performance_set(pxsys_reference_performance_option_t option,
                               bool enabled) {
    const char* key = option == PXSYS_REFERENCE_PERFORMANCE_OVERLAY
                          ? "overlay"
                          : option == PXSYS_REFERENCE_PERFORMANCE_LOG ? "log"
                          : option == PXSYS_REFERENCE_PXADB ? "pxadb" : NULL;
    nvs_handle_t handle;
    if (key == NULL || nvs_open("pxa_perf", NVS_READWRITE, &handle) != ESP_OK)
        return false;
    const esp_err_t result = nvs_set_u8(handle, key, enabled ? 1 : 0);
    const esp_err_t committed = result == ESP_OK ? nvs_commit(handle) : result;
    nvs_close(handle);
    if (committed != ESP_OK) return false;
    g_performance_loaded = 1;
    if (option == PXSYS_REFERENCE_PERFORMANCE_OVERLAY)
        g_performance_overlay = enabled;
    else if (option == PXSYS_REFERENCE_PERFORMANCE_LOG)
        g_performance_log = enabled;
    else
        g_pxa_debug_enabled = enabled;
    return true;
}

void pxa_board_performance_note_frame(void) {
    atomic_fetch_add_explicit(&g_frame_window_count, 1, memory_order_relaxed);
    (void)pxa_board_performance_refresh();
}

uint32_t pxa_board_performance_refresh(void) {
    // A completion worker and a renderer may sample concurrently. Never wait
    // for the other sampler, and never lose increments from the display task.
    const uint8_t state = atomic_fetch_or_explicit(
        &g_performance_sampling, SAMPLE_BUSY, memory_order_acquire);
    if (state & SAMPLE_BUSY)
        return atomic_load_explicit(&g_fps_x10, memory_order_relaxed);
    const uint32_t now = (uint32_t)esp_timer_get_time();
    const uint32_t elapsed = now - g_frame_window_started_us;
    if (!(state & SAMPLE_INITIALIZED)) {
        // The first completion establishes the interval, rather than being
        // counted as both its starting boundary and one elapsed frame.
        atomic_exchange_explicit(&g_frame_window_count, 0, memory_order_relaxed);
        g_frame_window_started_us = now;
    } else if (elapsed >= 1000000) {
        const uint32_t frames = atomic_exchange_explicit(
            &g_frame_window_count, 0, memory_order_relaxed);
        const uint32_t fps_x10 = (uint32_t)(frames * 10000000ULL / elapsed);
        atomic_store_explicit(&g_fps_x10, fps_x10, memory_order_relaxed);
        if (pxa_board_performance_get(PXSYS_REFERENCE_PERFORMANCE_LOG))
            ESP_LOGI("pxa_perf", "FPS=%lu.%lu frames=%lu window=%lums",
                     (unsigned long)(fps_x10 / 10),
                     (unsigned long)(fps_x10 % 10),
                     (unsigned long)frames,
                     (unsigned long)(elapsed / 1000));
        g_frame_window_started_us = now;
    }
    const uint32_t result = atomic_load_explicit(&g_fps_x10, memory_order_relaxed);
    atomic_store_explicit(&g_performance_sampling, SAMPLE_INITIALIZED, memory_order_release);
    return result;
}

static uint8_t glyph_row(char c, uint8_t row) {
    static const uint8_t numbers[10][5] = {
        {7,5,5,5,7},{2,6,2,2,7},{7,1,7,4,7},{7,1,7,1,7},{5,5,7,1,1},
        {7,4,7,1,7},{7,4,7,5,7},{7,1,2,2,2},{7,5,7,5,7},{7,5,7,1,7}};
    static const uint8_t f[5] = {7,4,6,4,4};
    static const uint8_t p[5] = {6,5,6,4,4};
    static const uint8_t s[5] = {7,4,7,1,7};
    if (c >= '0' && c <= '9') return numbers[c - '0'][row];
    if (c == '.') return row == 4 ? 2 : 0;
    return c == 'F' ? f[row] : c == 'P' ? p[row] : c == 'S' ? s[row] : 0;
}

void pxa_board_performance_draw_rgb565(uint16_t* pixels, uint16_t viewport_width,
                                       uint16_t viewport_height, uint32_t stride,
                                       int32_t origin_x, int32_t origin_y,
                                       uint16_t width, uint16_t height,
                                       bool byte_swapped) {
    char text[8] = {'F','P','S','0','0','.','0','\0'};
    const int32_t left = ((int32_t)viewport_width - PXA_BOARD_PERFORMANCE_OVERLAY_WIDTH) / 2;
    const uint16_t white = byte_swapped ? __builtin_bswap16(0xffff) : 0xffff;
    const uint16_t black = 0;
    if (!pxa_board_performance_get(PXSYS_REFERENCE_PERFORMANCE_OVERLAY) ||
        pixels == NULL) return;
    // Most scanout bands do not intersect this small overlay. Avoid walking
    // every glyph and background pixel for each such band. Order the positive
    // bounds first so even extreme signed origins cannot overflow the sums.
    if (width == 0 || height == 0 || origin_x >= left + PXA_BOARD_PERFORMANCE_OVERLAY_WIDTH ||
        origin_y >= PXA_BOARD_PERFORMANCE_OVERLAY_HEIGHT ||
        origin_x + (int32_t)width <= left || origin_y + (int32_t)height <= 0)
        return;
    const uint32_t fps_x10 = atomic_load_explicit(&g_fps_x10, memory_order_relaxed);
    text[3] = (char)('0' + (fps_x10 / 100) % 10);
    text[4] = (char)('0' + (fps_x10 / 10) % 10);
    text[6] = (char)('0' + fps_x10 % 10);
    for (int32_t y = 0; y < PXA_BOARD_PERFORMANCE_OVERLAY_HEIGHT && y < viewport_height; ++y)
        for (int32_t x = left; x < left + PXA_BOARD_PERFORMANCE_OVERLAY_WIDTH; ++x) {
        if (x >= 0 && x < viewport_width && x >= origin_x && y >= origin_y &&
            x < origin_x + width && y < origin_y + height)
            pixels[(size_t)(y - origin_y) * stride + x - origin_x] = black;
    }
    for (int32_t i = 0; text[i]; ++i) for (uint8_t y = 0; y < 5; ++y) {
        const uint8_t bits = glyph_row(text[i], y);
        for (uint8_t x = 0; x < 3; ++x) if (bits & (1u << (2 - x)))
            for (int sy = 0; sy < 2; ++sy) for (int sx = 0; sx < 2; ++sx) {
                const int32_t px = left + 3 + i * 7 + x * 2 + sx;
                const int32_t py = 3 + y * 2 + sy;
                if (px >= 0 && px < viewport_width && py < viewport_height &&
                    px >= origin_x && py >= origin_y &&
                    px < origin_x + width && py < origin_y + height)
                    pixels[(size_t)(py - origin_y) * stride + px - origin_x] = white;
            }
    }
}
