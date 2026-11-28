#include "pxa_board_api.h"
#include "pxa/display_rotation.h"
#include "sdkconfig.h"

#include <nvs.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <stdatomic.h>
#include <stddef.h>

static const pxa_board_port_t* g_board_port;
static _Atomic uint8_t g_display_rotation;
static pxsys_standard_system_t* g_display_system;

void pxa_board_display_attach_system(pxsys_standard_system_t* system) {
    g_display_system = system;
}

bool pxa_board_display_get_state(pxa_board_display_state_t* output) {
    if (!g_board_port || !output) return false;
    pxsys_display_profile_t profile;
    g_board_port->display_profile(g_board_port->context, &profile);
    output->native_width = profile.width;
    output->native_height = profile.height;
    output->rotation = g_display_rotation;
    output->supported_rotations = g_board_port->supported_rotations
        ? g_board_port->supported_rotations : 1;
    return profile.width && profile.height;
}

bool pxa_board_display_set_orientation(uint8_t preference) {
    pxa_board_display_state_t state;
    uint8_t rotation;
    if (!pxa_board_display_get_state(&state) ||
        !pxa_display_resolve_rotation(state.native_width, state.native_height,
            0, state.rotation, preference, state.supported_rotations, &rotation)) return false;
    if (rotation == state.rotation) return true;
    const pxa_board_port_t* board = g_board_port;
    if (!board->set_display_rotation) return false;
    if (board->lock_display && !board->lock_display(board->context, 1500)) return false;
    lv_lock();
    bool ok = board->set_display_rotation(board->context, rotation);
    if (ok) {
        g_display_rotation = rotation;
        if (g_display_system) {
            pxsys_display_profile_t profile;
            board->display_profile(board->context, &profile);
            pxsys_display_profile_t rotated;
            if (pxsys_display_profile_rotated(&profile, rotation, &rotated) == PXSYS_STATUS_OK)
                profile = rotated;
            (void)pxsys_display_service_update(pxsys_standard_system_display(g_display_system), &profile);
        }
    }
    lv_unlock();
    if (board->unlock_display) board->unlock_display(board->context);
    return ok;
}
static _Atomic uint8_t g_performance_loaded;
static _Atomic uint8_t g_performance_overlay;
static _Atomic uint8_t g_performance_log;
#ifndef CONFIG_PXADB_AUTOSTART
#define CONFIG_PXADB_AUTOSTART 0
#endif
static _Atomic uint8_t g_pxa_debug_enabled = CONFIG_PXADB_AUTOSTART;
/* These two flags share one byte, filling the old settings alignment gap. */
enum { SAMPLE_BUSY = 1, SAMPLE_INITIALIZED = 2 };
static _Atomic uint8_t g_performance_sampling;
static uint32_t g_frame_window_started_us;
static _Atomic uint32_t g_frame_window_count;
static _Atomic uint32_t g_fps_x10;
static _Atomic bool g_performance_light_theme;

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

bool pxa_board_wifi_get_enabled(void) {
    nvs_handle_t handle;
    uint8_t value = 1;
    if (nvs_open("pxa_user", NVS_READONLY, &handle) != ESP_OK)
        return true;
    const esp_err_t result = nvs_get_u8(handle, "wifi", &value);
    nvs_close(handle);
    return result != ESP_OK || value != 0;
}

bool pxa_board_wifi_set_enabled(bool enabled) {
    nvs_handle_t handle;
    if (nvs_open("pxa_user", NVS_READWRITE, &handle) != ESP_OK)
        return false;
    uint8_t saved;
    if (nvs_get_u8(handle, "wifi", &saved) == ESP_OK && saved == (enabled ? 1 : 0)) {
        nvs_close(handle);
        return true;
    }
    esp_err_t result = nvs_set_u8(handle, "wifi", enabled ? 1 : 0);
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    if (result != ESP_OK)
        ESP_LOGW("pxa_board", "Could not save Wi-Fi switch (error %d)", (int)result);
    return result == ESP_OK;
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
        case PXSYS_REFERENCE_PXADB_LAN:
            return false; /* The Wi-Fi listener is never persisted. */
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
    static const uint8_t digits[10][5] = {
        {7,5,5,5,7},{2,6,2,2,7},{7,1,7,4,7},{7,1,7,1,7},{5,5,7,1,1},
        {7,4,7,1,7},{7,4,7,5,7},{7,1,2,2,2},{7,5,7,5,7},{7,5,7,1,7}};
    static const uint8_t f[5] = {7,4,6,4,4}, p[5] = {6,5,6,4,4},
        s[5] = {7,4,7,1,7}, m[5] = {5,7,7,5,5}, r[5] = {6,5,6,5,5},
        t[5] = {7,2,2,2,2};
    if (c >= '0' && c <= '9') return digits[c - '0'][row];
    switch (c) {
        case '.': return row == 4 ? 2 : 0;
        case '-': return row == 2 ? 7 : 0;
        case 'F': return f[row];
        case 'P': return p[row];
        case 'S': return s[row];
        case 'M': return m[row];
        case 'R': return r[row];
        case 'T': return t[row];
        default: return 0;
    }
}

void pxa_board_performance_set_light_theme(bool light) {
    atomic_store_explicit(&g_performance_light_theme, light, memory_order_relaxed);
}

typedef struct {
    uint16_t* pixels;
    int32_t left, top, right, bottom;
    ptrdiff_t stride;
    bool rotated, byte_swapped;
} performance_target_t;

static int32_t minimum(int32_t a, int32_t b) { return a < b ? a : b; }
static int32_t maximum(int32_t a, int32_t b) { return a > b ? a : b; }

static void performance_fill(const performance_target_t* target, int32_t x,
                             int32_t y, int32_t width, int32_t height, uint16_t color) {
    const int32_t left = maximum(x, target->left);
    const int32_t top = maximum(y, target->top);
    const int32_t right = minimum(x + width, target->right);
    const int32_t bottom = minimum(y + height, target->bottom);
    if (left >= right || top >= bottom) return;
    if (target->byte_swapped) color = __builtin_bswap16(color);
    if (target->rotated) {
        for (int32_t column = left; column < right; ++column) {
            uint16_t* row = target->pixels + column * target->stride - (bottom - 1);
            for (int32_t i = 0; i < bottom - top; ++i) row[i] = color;
        }
    } else {
        for (int32_t line = top; line < bottom; ++line) {
            uint16_t* row = target->pixels + (line - target->top) * target->stride +
                            left - target->left;
            for (int32_t i = 0; i < right - left; ++i) row[i] = color;
        }
    }
}

static void performance_text(const performance_target_t* target, int32_t x,
                             const char* text, uint16_t color) {
    for (; *text; ++text, x += 7) {
        if (*text == ' ' || x + 6 <= target->left || x >= target->right) continue;
        for (uint8_t y = 0; y < 5; ++y) {
            const uint8_t bits = glyph_row(*text, y);
            if (bits == 7) {
                performance_fill(target, x, 3 + 2 * y, 6, 2, color);
                continue;
            }
            for (uint8_t column = 0; column < 3; ++column)
                if (bits & (4u >> column))
                    performance_fill(target, x + column * 2, 3 + 2 * y, 2, 2, color);
        }
    }
}

static void format_tenths(char* text, uint32_t value) {
    if (value > 9999) value = 9999;
    text[0] = value >= 1000 ? (char)('0' + value / 1000) : ' ';
    text[1] = value >= 100 ? (char)('0' + value / 100 % 10) : ' ';
    text[2] = (char)('0' + value / 10 % 10);
    text[3] = '.';
    text[4] = (char)('0' + value % 10);
}

#define HUD_RGB565(rgb) (((rgb) >> 8 & 0xf800) | ((rgb) >> 5 & 0x07e0) | ((rgb) >> 3 & 0x001f))
static void draw_performance(const performance_target_t* target, uint32_t fps_x10,
                             const char* details) {
    static const uint16_t colors[2][3] = {
        {HUD_RGB565(0x1b2432), HUD_RGB565(0xeef4ff), HUD_RGB565(0xadc8ff)},
        {HUD_RGB565(0xebf1fc), HUD_RGB565(0x243449), HUD_RGB565(0x355a9f)},
    };
    const uint16_t* palette = colors[atomic_load_explicit(&g_performance_light_theme,
                                                          memory_order_relaxed)];
    char fps[6] = {0};
    format_tenths(fps, fps_x10);
    performance_fill(target, 0, 0, PXA_BOARD_PERFORMANCE_OVERLAY_WIDTH,
                     PXA_BOARD_PERFORMANCE_OVERLAY_HEIGHT, palette[0]);
    performance_text(target, 8, "FPS", palette[1]);
    performance_text(target, 31, fps, palette[2]);
    performance_text(target, 74, details, palette[1]);
}
#undef HUD_RGB565

void pxa_board_performance_draw_rgb565(uint16_t* pixels, uint16_t viewport_width,
                                       uint16_t viewport_height, uint32_t stride,
                                       int32_t origin_x, int32_t origin_y,
                                       uint16_t width, uint16_t height,
                                       bool byte_swapped) {
    const int32_t left = ((int32_t)viewport_width - PXA_BOARD_PERFORMANCE_OVERLAY_WIDTH) / 2;
    const int32_t right = left + PXA_BOARD_PERFORMANCE_OVERLAY_WIDTH;
    /* Check positive bounds first, before adding a length to either origin. */
    if (pixels == NULL || width == 0 || height == 0 || origin_x >= right ||
        origin_y >= PXA_BOARD_PERFORMANCE_OVERLAY_HEIGHT ||
        origin_x + (int32_t)width <= left || origin_y + (int32_t)height <= 0 ||
        !pxa_board_performance_get(PXSYS_REFERENCE_PERFORMANCE_OVERLAY)) return;
    const int32_t clip_left = maximum(0, maximum(origin_x, left));
    const int32_t clip_top = maximum(0, origin_y);
    const int32_t clip_right = minimum(viewport_width, minimum(origin_x + width, right));
    const int32_t clip_bottom = minimum(viewport_height,
        minimum(origin_y + height, PXA_BOARD_PERFORMANCE_OVERLAY_HEIGHT));
    if (clip_left >= clip_right || clip_top >= clip_bottom) return;
    const performance_target_t target = {
        .pixels = pixels + (size_t)(clip_top - origin_y) * stride + (clip_left - origin_x),
        .left = clip_left - left, .top = clip_top, .right = clip_right - left,
        .bottom = clip_bottom, .stride = stride, .byte_swapped = byte_swapped,
    };
    const uint32_t fps_x10 = atomic_load_explicit(&g_fps_x10, memory_order_relaxed);
    char details[] = " --.-MS";
    if (fps_x10 != 0) format_tenths(details, (100000u + fps_x10 / 2u) / fps_x10);
    draw_performance(&target, fps_x10, details);
}

void pxa_board_performance_draw_panel_rgb565(uint16_t* pixels, uint16_t width,
                                             uint16_t height, uint32_t fps_x10,
                                             uint32_t rotate_us, uint32_t transfer_us) {
    if (pixels == NULL || width < PXA_BOARD_PERFORMANCE_OVERLAY_WIDTH ||
        height < PXA_BOARD_PERFORMANCE_OVERLAY_HEIGHT) return;
    const size_t left = (width - PXA_BOARD_PERFORMANCE_OVERLAY_WIDTH) / 2u;
    const performance_target_t target = {
        .pixels = pixels + left * height + height - 1u,
        .right = PXA_BOARD_PERFORMANCE_OVERLAY_WIDTH,
        .bottom = PXA_BOARD_PERFORMANCE_OVERLAY_HEIGHT,
        .stride = height, .rotated = true, .byte_swapped = true,
    };
    const uint32_t rotation = rotate_us < 99000u ? rotate_us / 1000u : 99u;
    const uint32_t transfer = transfer_us < 99000u ? transfer_us / 1000u : 99u;
    char details[] = {'R', rotation >= 10 ? (char)('0' + rotation / 10) : ' ',
                      (char)('0' + rotation % 10), ' ',
                      'T', transfer >= 10 ? (char)('0' + transfer / 10) : ' ',
                      (char)('0' + transfer % 10), '\0'};
    draw_performance(&target, fps_x10, details);
}
