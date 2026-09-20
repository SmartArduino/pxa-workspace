#include "pxa_board_api.h"

#include <nvs.h>
#include <esp_log.h>
#include <esp_timer.h>

static const pxa_board_port_t* g_board_port;
static uint8_t g_performance_loaded;
static uint8_t g_performance_overlay;
static uint8_t g_performance_log;
static uint8_t g_pxa_debug_enabled = 1;
static uint32_t g_frame_window_started_us;
static uint32_t g_frame_window_count;
static uint32_t g_fps_x10;

static void load_performance_settings(void) {
    nvs_handle_t handle;
    uint8_t value;
    if (g_performance_loaded) return;
    g_performance_loaded = 1;
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
        port->display_profile == NULL) {
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
    const uint32_t now = (uint32_t)esp_timer_get_time();
    if (g_frame_window_started_us == 0) g_frame_window_started_us = now;
    ++g_frame_window_count;
    const uint32_t elapsed = now - g_frame_window_started_us;
    if (elapsed < 1000000) return;
    g_fps_x10 = (uint32_t)(g_frame_window_count * 10000000ULL / elapsed);
    if (pxa_board_performance_get(PXSYS_REFERENCE_PERFORMANCE_LOG))
        ESP_LOGI("pxa_perf", "FPS=%lu.%lu frames=%lu window=%lums",
                 (unsigned long)(g_fps_x10 / 10),
                 (unsigned long)(g_fps_x10 % 10),
                 (unsigned long)g_frame_window_count,
                 (unsigned long)(elapsed / 1000));
    g_frame_window_started_us = now;
    g_frame_window_count = 0;
}

static uint8_t glyph_row(char c, uint8_t row) {
    static const uint8_t numbers[10][5] = {
        {7,5,5,5,7},{2,6,2,2,7},{7,1,7,4,7},{7,1,7,1,7},{5,5,7,1,1},
        {7,4,7,1,7},{7,4,7,5,7},{7,1,2,2,2},{7,5,7,5,7},{7,5,7,1,7}};
    static const uint8_t f[5] = {7,4,6,4,4};
    static const uint8_t p[5] = {6,5,6,4,4};
    static const uint8_t s[5] = {7,4,7,1,7};
    if (c >= '0' && c <= '9') return numbers[c - '0'][row];
    return c == 'F' ? f[row] : c == 'P' ? p[row] : c == 'S' ? s[row] : 0;
}

void pxa_board_performance_draw_rgb565(uint16_t* pixels, uint16_t viewport_width,
                                       uint16_t viewport_height, uint32_t stride,
                                       int32_t origin_x, int32_t origin_y,
                                       uint16_t width, uint16_t height,
                                       bool byte_swapped) {
    char text[8] = {'F','P','S','0','0','.','0','\0'};
    const int32_t left = ((int32_t)viewport_width - 60) / 2;
    const uint16_t white = byte_swapped ? __builtin_bswap16(0xffff) : 0xffff;
    const uint16_t black = 0;
    if (!pxa_board_performance_get(PXSYS_REFERENCE_PERFORMANCE_OVERLAY) ||
        pixels == NULL) return;
    text[3] = (char)('0' + (g_fps_x10 / 100) % 10);
    text[4] = (char)('0' + (g_fps_x10 / 10) % 10);
    text[6] = (char)('0' + g_fps_x10 % 10);
    for (int32_t y = 0; y < 16; ++y) for (int32_t x = left; x < left + 60; ++x) {
        if (x >= origin_x && y >= origin_y && x < origin_x + width && y < origin_y + height)
            pixels[(size_t)(y - origin_y) * stride + x - origin_x] = black;
    }
    for (int32_t i = 0; text[i]; ++i) for (uint8_t y = 0; y < 5; ++y) {
        const uint8_t bits = glyph_row(text[i], y);
        for (uint8_t x = 0; x < 3; ++x) if (bits & (1u << (2 - x)))
            for (int sy = 0; sy < 2; ++sy) for (int sx = 0; sx < 2; ++sx) {
                const int32_t px = left + 3 + i * 7 + x * 2 + sx;
                const int32_t py = 3 + y * 2 + sy;
                if (px >= origin_x && py >= origin_y && px < origin_x + width && py < origin_y + height)
                    pixels[(size_t)(py - origin_y) * stride + px - origin_x] = white;
            }
    }
}
