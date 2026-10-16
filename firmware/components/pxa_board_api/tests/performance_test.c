#include "pxa_board_api.h"
#include <nvs.h>
#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static _Atomic int64_t now_us;
static uint8_t overlay = 1;
int64_t esp_timer_get_time(void) { return atomic_load(&now_us); }
esp_err_t nvs_open(const char* name, int mode, nvs_handle_t* out) {
    (void)name; (void)mode; *out = 1; return ESP_OK;
}
esp_err_t nvs_get_u8(nvs_handle_t handle, const char* key, uint8_t* out) {
    (void)handle; *out = strcmp(key, "overlay") == 0 ? overlay : 0; return ESP_OK;
}
esp_err_t nvs_set_u8(nvs_handle_t handle, const char* key, uint8_t value) {
    (void)handle; if (strcmp(key, "overlay") == 0) overlay = value; return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle) { (void)handle; return ESP_OK; }
void nvs_close(nvs_handle_t handle) { (void)handle; }

static void test_rate(void) {
    // Start exactly at zero: the first displayed frame starts the interval.
    atomic_store(&now_us, 0);
    pxa_board_performance_note_frame();
    assert(pxa_board_performance_refresh() == 0);
    for (int i = 1; i <= 30; ++i) {
        atomic_store(&now_us, i * 33334);
        pxa_board_performance_note_frame();
    }
    assert(pxa_board_performance_refresh() == 299);
    // Long idle and the 32-bit microsecond rollover must not retain old FPS.
    atomic_store(&now_us, UINT32_MAX + INT64_C(1000000));
    assert(pxa_board_performance_refresh() == 0);
    atomic_fetch_add(&now_us, 1000000);
    assert(pxa_board_performance_refresh() == 0);
}

static void test_pixels(void) {
    enum { W = 480, H = 32 };
    uint16_t full[W * H], tiled[W * H];
    for (int i = 0; i < W * H; ++i) full[i] = tiled[i] = 0x1234;
    pxa_board_performance_draw_rgb565(full, W, H, W, 0, 0, W, H, false);
    // Decimal point of FPS00.0 must be visible and not confused with a space.
    assert(full[11 * W + 250] == 0xffff);
    assert(full[3 * W + 250] == 0);
    // Partial scanout bands, arbitrary strides and byte orders reproduce the
    // same image without touching pixels outside the 60x16 HUD rectangle.
    for (int y = 0; y < H; y += 3) for (int x = 0; x < W; x += 19) {
        int w = x + 19 > W ? W - x : 19;
        int h = y + 3 > H ? H - y : 3;
        pxa_board_performance_draw_rgb565(tiled + y * W + x, W, H, W,
                                         x, y, w, h, true);
    }
    assert(memcmp(full, tiled, sizeof(full)) == 0);
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x)
        if (y >= 16 || x < 210 || x >= 270) assert(full[y * W + x] == 0x1234);
    uint16_t guard = 0xabcd;
    pxa_board_performance_draw_rgb565(&guard, W, H, 1, INT32_MIN, 0, 1, 1, false);
    pxa_board_performance_draw_rgb565(&guard, W, H, 1, INT32_MAX, 0, 1, 1, false);
    assert(guard == 0xabcd);
    assert(pxa_board_performance_set(PXSYS_REFERENCE_PERFORMANCE_OVERLAY, false));
    pxa_board_performance_draw_rgb565(&guard, W, H, 1, 250, 11, 1, 1, false);
    assert(guard == 0xabcd);
}

static void* count_frames(void* unused) {
    (void)unused;
    for (int i = 0; i < 1000; ++i) pxa_board_performance_note_frame();
    return NULL;
}

static void test_concurrent_completions(void) {
    pthread_t threads[4];
    for (int i = 0; i < 4; ++i) assert(pthread_create(&threads[i], NULL, count_frames, NULL) == 0);
    for (int i = 0; i < 1000; ++i) (void)pxa_board_performance_refresh();
    for (int i = 0; i < 4; ++i) assert(pthread_join(threads[i], NULL) == 0);
    atomic_fetch_add(&now_us, 1000000);
    assert(pxa_board_performance_refresh() == 40000);
}

int main(void) {
    test_rate();
    test_pixels();
    test_concurrent_completions();
    puts("Board performance: timing, idle, rollover, decimal, tiles and concurrent completions passed");
}
