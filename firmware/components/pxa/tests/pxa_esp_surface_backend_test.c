#include "pxa_test_platform.h"

#include <malloc.h>
#include <string.h>

#define ESP_PLATFORM 1
#define PXA_ESP_SURFACE_GUEST_MAPPING_SUPPORTED 1
#include "esp_cache.h"
static int surface_test_cache_msync(void *address, size_t size, int flags);
#define esp_cache_msync surface_test_cache_msync
#include "../src/services/pxa_esp_resource_memory.c"
#include "../src/services/pxa_esp_surface.c"

struct frame_alignment_probe { char byte; uint64_t value; };
enum { frame_u64_alignment = offsetof(struct frame_alignment_probe, value) };
typedef char raster_ready_tick_fits_existing_padding[
    offsetof(pxa_esp_surface_frame_t, frame_id) ==
    ((offsetof(pxa_esp_surface_frame_t, suppress_guest_alpha) + 1 +
      frame_u64_alignment - 1) / frame_u64_alignment * frame_u64_alignment)
    ? 1 : -1];

/* This fixture has no file worker; the actual cache reclaimer is exercised
 * by assets_backend and the product runtime tests. */
size_t pxa_esp_assets_trim(uint8_t cls, size_t bytes) { (void)cls; (void)bytes; return 0; }
static unsigned lock_depth;
static unsigned allocations;
static unsigned fail_next_malloc;
static bool fail_readback;
static size_t last_aligned_allocation_size;
static unsigned last_malloc_caps;
static unsigned notifications;
static unsigned raster_notifications;
static unsigned release_notifications;
static int64_t test_now_us;

static void (*during_raster)(void);
int64_t esp_timer_get_time(void) {
    if (during_raster != NULL && lock_depth == 0 && g_surface != NULL &&
        g_surface->raster_draw_rendering != PXA_ESP_SURFACE_NONE) {
        void (*callback)(void) = during_raster;
        during_raster = NULL;
        callback();
    }
    return test_now_us;
}

void test_enter(void) { assert(lock_depth++ == 0); }
void test_leave(void) { assert(lock_depth-- == 1); }
void test_log(const char *tag, const char *format, ...) {
    (void)tag;
    (void)format;
}
void *heap_caps_malloc(size_t size, unsigned caps) {
    last_malloc_caps = caps;
    assert(lock_depth == 0);
    if (fail_next_malloc != 0) {
        --fail_next_malloc;
        return NULL;
    }
    void *memory = malloc(size);
    if (memory != NULL) ++allocations;
    return memory;
}
void *heap_caps_calloc(size_t count, size_t size, unsigned caps) {
    (void)caps;
    void *memory = calloc(count, size);
    if (memory != NULL) ++allocations;
    return memory;
}
void *heap_caps_aligned_alloc(size_t alignment, size_t size, unsigned caps) {
    (void)alignment;
    last_aligned_allocation_size = size;
    return heap_caps_malloc(size, caps);
}
void *heap_caps_aligned_calloc(size_t alignment, size_t count, size_t size,
                               unsigned caps) {
    last_malloc_caps = caps;
    assert(lock_depth == 0);
    if (fail_readback) {
        fail_readback = false;
        return NULL;
    }
    void *memory = memalign(alignment, count * size);
    if (memory != NULL) { ++allocations; memset(memory, 0, count * size); }
    return memory;
}
static int surface_test_cache_msync(void *address, size_t size, int flags) {
    assert((uintptr_t)address % PXA_ESP_SURFACE_CACHE_ALIGNMENT == 0);
    assert(size != 0 && size % PXA_ESP_SURFACE_CACHE_ALIGNMENT == 0);
    assert(size <= malloc_usable_size(address));
    assert(flags == ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    return 0;
}
void heap_caps_free(void *memory) {
    assert(lock_depth == 0);
    if (memory != NULL) {
        assert(allocations != 0);
        --allocations;
    }
    free(memory);
}
size_t heap_caps_get_allocated_size(void *memory) {
    return malloc_usable_size(memory);
}

static void ready(void *context) {
    assert(context == &notifications && lock_depth == 0);
    ++notifications;
}

static void raster_ready(void *context) {
    assert(context == &raster_notifications && lock_depth == 0);
    ++raster_notifications;
}

static void release_ready(void *context) {
    assert(context == &release_notifications && lock_depth == 0);
    ++release_notifications;
}

static bool ui_alpha_plane(void *context,
                           pxa_esp_surface_ui_alpha_plane_t *plane) {
    static const uint16_t pixels[] = {0xf800, 0x07e0, 0x001f, 0xffff};
    static const uint8_t alpha[] = {0, 128, 255, 64};
    assert(context == &notifications);
    memset(plane, 0, sizeof(*plane));
    plane->pixels = pixels;
    plane->alpha = alpha;
    plane->pixel_stride_bytes = 4;
    plane->alpha_stride_bytes = 2;
    plane->width = 2;
    plane->height = 2;
    plane->opacity = 255;
    plane->visible = 1;
    return true;
}

static bool empty_ui_alpha_plane(void *context,
                                 pxa_esp_surface_ui_alpha_plane_t *plane) {
    assert(context == &notifications);
    memset(plane, 0, sizeof(*plane));
    return true;
}

static bool missing_ui_alpha_plane(void *context,
                                   pxa_esp_surface_ui_alpha_plane_t *plane) {
    assert(context == &notifications);
    memset(plane, 0, sizeof(*plane));
    plane->visible = 1;
    return true;
}

static bool failed_ui_alpha_plane(void *context,
                                  pxa_esp_surface_ui_alpha_plane_t *plane) {
    assert(context == &notifications);
    (void)plane;
    return false;
}

#include "../../../../deps/pxa-system/libpxa/tests/raster_snapshot_scenario.h"

static void replace_during_raster(void) {
    uint8_t palette[PXA_RASTER_UPLOAD_HEADER_BYTES + 512] = {0};
    snapshot_upload_header(palette, PXA_RASTER_UPLOAD_PALETTE_RGB565, 256, 1);
    pxa_write_u16(palette + PXA_RASTER_UPLOAD_HEADER_BYTES + 2, 0x07e0);
    pxa_write_u16(palette + PXA_RASTER_UPLOAD_HEADER_BYTES + 4, 0x001f);
    assert(raster_upload_surface(NULL, (uint64_t)(uintptr_t)g_surface,
                                  palette, sizeof(palette)) == PXA_STATUS_OK);
}

static void snapshot_present(void *context, uint64_t id, uint16_t color) {
    pxa_esp_surface_frame_t frame;
    const uint16_t *pixels;
    (void)context;
    if (id == 1) during_raster = replace_during_raster;
    assert(pxa_esp_surface_acquire_latest_for_direct(&frame));
    assert(during_raster == NULL);
    assert(frame.frame_id == id);
    pixels = (const uint16_t *)frame.pixels;
    for (unsigned i = 0; i < 16; ++i) assert(pixels[i] == color);
    pxa_esp_surface_release_frame(frame.lease);
}

static void close_during_raster(void) {
    close_surface(NULL, (uint64_t)(uintptr_t)g_surface);
}

static pxa_game_render_backend_t *replacement_backend;
static uint64_t replacement_surface;
static void replace_surface_during_raster(void) {
    uint8_t draw[PXA_RASTER_DRAW_HEADER_BYTES + PXA_RASTER_CLEAR_BYTES];
    uint8_t palette[PXA_RASTER_UPLOAD_HEADER_BYTES + 512] = {0};
    pxa_game_render_desc_t desc = {4, 4, 3, 0, PXA_GAME_RENDER_SCRATCH_NONE, 4096};
    uint32_t capabilities;
    memcpy(draw, g_surface->raster_draw_lists[g_surface->raster_draw_rendering], sizeof(draw));
    close_surface(NULL, (uint64_t)(uintptr_t)g_surface);
    assert(replacement_backend->create(NULL, &desc, &replacement_surface, &capabilities) == 0);
    snapshot_upload_header(palette, PXA_RASTER_UPLOAD_PALETTE_RGB565, 256, 1);
    assert(replacement_backend->upload(NULL, replacement_surface, palette, sizeof(palette)) == 0);
    assert(replacement_backend->submit(NULL, replacement_surface, draw, sizeof(draw)) == 0);
    // A new surface must not overwrite the old renderer's global worker job.
    assert(!pxa_esp_surface_prepare_latest_raster());
    assert(pxa_esp_surface_has_pending_raster_draw());
}

static void test_close_while_rendering(pxa_game_render_backend_t *backend) {
    pxa_game_render_desc_t desc = {4, 4, 3, 0, PXA_GAME_RENDER_SCRATCH_NONE, 4096};
    uint64_t surface;
    uint32_t capabilities;
    uint8_t palette[PXA_RASTER_UPLOAD_HEADER_BYTES + 512] = {0};
    uint8_t draw[PXA_RASTER_DRAW_HEADER_BYTES + PXA_RASTER_CLEAR_BYTES] = {0};
    pxa_esp_surface_frame_t frame;
    assert(backend->create(NULL, &desc, &surface, &capabilities) == 0);
    snapshot_upload_header(palette, PXA_RASTER_UPLOAD_PALETTE_RGB565, 256, 1);
    assert(backend->upload(NULL, surface, palette, sizeof(palette)) == 0);
    pxa_write_u32(draw, PXA_RASTER_DRAW_MAGIC);
    pxa_write_u16(draw + 4, PXA_RASTER_ABI_MAJOR);
    pxa_write_u16(draw + 6, PXA_RASTER_ABI_MINOR);
    pxa_write_u32(draw + 8, sizeof(draw));
    pxa_write_u32(draw + 16, 1);
    pxa_write_u64(draw + 20, 1);
    draw[PXA_RASTER_DRAW_HEADER_BYTES] = PXA_RASTER_RECORD_CLEAR_RGB565;
    pxa_write_u16(draw + PXA_RASTER_DRAW_HEADER_BYTES + 2, PXA_RASTER_CLEAR_BYTES);
    assert(backend->submit(NULL, surface, draw, sizeof(draw)) == 0);
    during_raster = close_during_raster;
    assert(!pxa_esp_surface_acquire_latest_native_for_direct(&frame));
    assert(during_raster == NULL && allocations == 0);
    pxa_esp_surface_memory_info_t memory = {0};
    pxa_esp_surface_memory_snapshot(&memory);
    assert(memory.frame_bytes == 0 && memory.scratch_bytes == 0 &&
           memory.mailbox_bytes == 0 && memory.probe_bytes == 0);
}

static void test_replace_while_rendering(pxa_game_render_backend_t *backend) {
    pxa_game_render_desc_t desc = {4, 4, 3, 0, PXA_GAME_RENDER_SCRATCH_NONE, 4096};
    uint64_t surface;
    uint32_t capabilities;
    uint8_t palette[PXA_RASTER_UPLOAD_HEADER_BYTES + 512] = {0};
    uint8_t draw[PXA_RASTER_DRAW_HEADER_BYTES + PXA_RASTER_CLEAR_BYTES] = {0};
    pxa_esp_surface_frame_t frame;
    assert(backend->create(NULL, &desc, &surface, &capabilities) == 0);
    snapshot_upload_header(palette, PXA_RASTER_UPLOAD_PALETTE_RGB565, 256, 1);
    assert(backend->upload(NULL, surface, palette, sizeof(palette)) == 0);
    pxa_write_u32(draw, PXA_RASTER_DRAW_MAGIC);
    pxa_write_u16(draw + 4, PXA_RASTER_ABI_MAJOR);
    pxa_write_u16(draw + 6, PXA_RASTER_ABI_MINOR);
    pxa_write_u32(draw + 8, sizeof(draw));
    pxa_write_u32(draw + 16, 1);
    pxa_write_u64(draw + 20, 1);
    draw[PXA_RASTER_DRAW_HEADER_BYTES] = PXA_RASTER_RECORD_CLEAR_RGB565;
    pxa_write_u16(draw + PXA_RASTER_DRAW_HEADER_BYTES + 2, PXA_RASTER_CLEAR_BYTES);
    assert(backend->submit(NULL, surface, draw, sizeof(draw)) == 0);
    replacement_backend = backend;
    during_raster = replace_surface_during_raster;
    assert(!pxa_esp_surface_acquire_latest_for_direct(&frame));
    assert(during_raster == NULL && !g_materialize_active);
    assert(pxa_esp_surface_prepare_latest_raster());
    assert(pxa_esp_surface_acquire_prepared(&frame, true));
    assert(frame.frame_id == 1);
    pxa_esp_surface_release_frame(frame.lease);
    close_surface(NULL, replacement_surface);
    assert(allocations == 0);
    pxa_esp_surface_memory_info_t memory = {0};
    pxa_esp_surface_memory_snapshot(&memory);
    assert(memory.frame_bytes == 0 && memory.scratch_bytes == 0 &&
           memory.mailbox_bytes == 0 && memory.probe_bytes == 0);
}

/* Mimic an asynchronous PPA holding a frame while the raster worker prepares
 * the next one. Two buffers back-pressure safely; three overlap all stages. */
static void test_prepared_pipeline(pxa_game_render_backend_t *backend) {
    pxa_esp_surface_set_raster_ready_callback(raster_ready, &raster_notifications);
    for (unsigned count = 2; count <= 3; ++count) {
        pxa_game_render_desc_t desc = {4, 4, count, 0, PXA_GAME_RENDER_SCRATCH_NONE, 4096};
        uint64_t surface;
        uint32_t capabilities;
        uint8_t palette[PXA_RASTER_UPLOAD_HEADER_BYTES + 512] = {0};
        uint8_t draw[PXA_RASTER_DRAW_HEADER_BYTES + PXA_RASTER_CLEAR_BYTES] = {0};
        pxa_esp_surface_frame_t first, second, third;
        assert(backend->create(NULL, &desc, &surface, &capabilities) == 0);
        snapshot_upload_header(palette, PXA_RASTER_UPLOAD_PALETTE_RGB565, 256, 1);
        assert(backend->upload(NULL, surface, palette, sizeof(palette)) == 0);
        pxa_write_u32(draw, PXA_RASTER_DRAW_MAGIC);
        pxa_write_u16(draw + 4, PXA_RASTER_ABI_MAJOR);
        pxa_write_u16(draw + 6, PXA_RASTER_ABI_MINOR);
        pxa_write_u32(draw + 8, sizeof(draw));
        pxa_write_u32(draw + 16, 1);
        draw[PXA_RASTER_DRAW_HEADER_BYTES] = PXA_RASTER_RECORD_CLEAR_RGB565;
        pxa_write_u16(draw + PXA_RASTER_DRAW_HEADER_BYTES + 2, PXA_RASTER_CLEAR_BYTES);
        pxa_write_u64(draw + 20, 1);
        pxa_write_u16(draw + PXA_RASTER_DRAW_HEADER_BYTES + 4, 0x1111);
        const unsigned before_submit = notifications;
        const unsigned before_raster = raster_notifications;
        assert(backend->submit(NULL, surface, draw, sizeof(draw)) == 0);
        assert(notifications == before_submit && raster_notifications == before_raster + 1);
        assert(pxa_esp_surface_has_pending_raster_draw());
        assert(!pxa_esp_surface_acquire_prepared(&first, true));
        assert(pxa_esp_surface_prepare_latest_raster());
        assert(!pxa_esp_surface_has_pending_raster_draw());
        assert(pxa_esp_surface_acquire_prepared(&first, true));
        pxa_write_u64(draw + 20, 2);
        pxa_write_u16(draw + PXA_RASTER_DRAW_HEADER_BYTES + 4, 0x2222);
        assert(backend->submit(NULL, surface, draw, sizeof(draw)) == 0);
        assert(pxa_esp_surface_prepare_latest_raster());
        for (unsigned i = 0; i < 16; ++i)
            assert(((const uint16_t *)first.pixels)[i] == 0x1111);
        pxa_esp_surface_release_frame(first.lease);
        assert(pxa_esp_surface_acquire_prepared(&second, true));
        assert(second.frame_id == 2);
        pxa_write_u64(draw + 20, 3);
        pxa_write_u16(draw + PXA_RASTER_DRAW_HEADER_BYTES + 4, 0x3333);
        assert(backend->submit(NULL, surface, draw, sizeof(draw)) == 0);
        assert(pxa_esp_surface_prepare_latest_raster() == (count == 3));
        for (unsigned i = 0; i < 16; ++i)
            assert(((const uint16_t *)second.pixels)[i] == 0x2222);
        pxa_esp_surface_release_frame(second.lease);
        if (count == 2) assert(pxa_esp_surface_prepare_latest_raster());
        assert(pxa_esp_surface_acquire_prepared(&third, true));
        assert(third.frame_id == 3);
        for (unsigned i = 0; i < 16; ++i)
            assert(((const uint16_t *)third.pixels)[i] == 0x3333);
        pxa_esp_surface_release_frame(third.lease);
        pxa_write_u64(draw + 20, 4);
        assert(backend->submit(NULL, surface, draw, sizeof(draw)) == 0);
        const unsigned before_hidden = notifications;
        const unsigned before_hidden_raster = raster_notifications;
        pxa_esp_surface_set_presentation_visible(false);
        pxa_write_u64(draw + 20, 5);
        assert(backend->submit(NULL, surface, draw, sizeof(draw)) == 0);
        // Hidden game submissions must not schedule redraws over Recents.
        assert(notifications == before_hidden && raster_notifications == before_hidden_raster);
        assert(!pxa_esp_surface_has_visible_surface());
        assert(!pxa_esp_surface_has_pending_frame());
        assert(!pxa_esp_surface_prepare_latest_raster());
        assert(!pxa_esp_surface_acquire_prepared(&first, false));
        assert(!pxa_esp_surface_acquire_prepared_with_alpha(&first));
        assert(pxa_esp_surface_has_pending_raster_draw());
        pxa_esp_surface_present_info_t info;
        assert(pxa_esp_surface_get_present_info(&info) && !info.visible);
        // Restoring a gesture snapshot must preserve independent host/lock gates.
        pxa_esp_surface_set_host_visible(false);
        pxa_esp_surface_set_presentation_visible(true);
        assert(!pxa_esp_surface_has_visible_surface());
        pxa_esp_surface_set_display_unlocked(false);
        pxa_esp_surface_set_host_visible(true);
        assert(!pxa_esp_surface_has_visible_surface());
        pxa_esp_surface_set_display_unlocked(true);
        assert(pxa_esp_surface_has_visible_surface());
        assert(!pxa_esp_surface_try_resume_direct_scanout(3));
        assert(pxa_esp_surface_prepare_latest_raster());
        assert(pxa_esp_surface_acquire_prepared(&first, false));
        assert(first.frame_id == 5);
        pxa_esp_surface_release_frame(first.lease);
        assert(!pxa_esp_surface_acquire_prepared_with_alpha(&first));
        pxa_write_u64(draw + 20, 6);
        assert(backend->submit(NULL, surface, draw, sizeof(draw)) == 0);
        assert(pxa_esp_surface_prepare_latest_raster());
        assert(pxa_esp_surface_acquire_prepared_with_alpha(&first));
        assert(first.frame_id == 6);
        pxa_esp_surface_release_frame(first.lease);
        close_surface(NULL, surface);
        assert(!g_materialize_active && allocations == 0);
    }
    pxa_esp_surface_set_raster_ready_callback(NULL, NULL);
}

/* Commands are consumed before submit returns, including on close/rebind.
 * No command mailbox exists; framebuffer leases still impose backpressure. */
static void test_borrowed_submit(pxa_game_render_backend_t *backend) {
    pxa_game_render_desc_t desc = {4, 4, 2, PXA_GAME_RENDER_FLAG_BORROW_COMMANDS,
                                  PXA_GAME_RENDER_SCRATCH_DEPTH16, 4096};
    uint64_t surface;
    uint32_t capabilities;
    uint8_t palette[PXA_RASTER_UPLOAD_HEADER_BYTES + 512] = {0};
    uint8_t draw[PXA_RASTER_DRAW_HEADER_BYTES + PXA_RASTER_CLEAR_BYTES] = {0};
    pxa_esp_surface_frame_t first, second, third;
    pxa_esp_surface_memory_info_t memory;
    assert(backend->create(NULL, &desc, &surface, &capabilities) == 0);
    pxa_esp_surface_memory_snapshot(&memory);
    /* Native malloc may round a 32-byte depth plane up; ASan reports the
     * requested size. The telemetry counts the allocator's actual storage. */
    assert(memory.mailbox_bytes == 0 && memory.scratch_bytes >= 32);
    assert(memory.scratch_bytes ==
           heap_caps_get_allocated_size(g_surface->raster_depth_buffer));
    for (unsigned i = 0; i < PXA_ESP_RASTER_MAILBOX_SLOTS; ++i)
        assert(g_surface->raster_draw_lists[i] == NULL);
    snapshot_upload_header(palette, PXA_RASTER_UPLOAD_PALETTE_RGB565, 256, 1);
    assert(backend->upload(NULL, surface, palette, sizeof(palette)) == 0);
    pxa_write_u32(draw, PXA_RASTER_DRAW_MAGIC);
    pxa_write_u16(draw + 4, PXA_RASTER_ABI_MAJOR);
    pxa_write_u16(draw + 6, PXA_RASTER_ABI_MINOR);
    pxa_write_u32(draw + 8, sizeof(draw));
    pxa_write_u32(draw + 16, 1);
    draw[PXA_RASTER_DRAW_HEADER_BYTES] = PXA_RASTER_RECORD_CLEAR_RGB565;
    pxa_write_u16(draw + PXA_RASTER_DRAW_HEADER_BYTES + 2, PXA_RASTER_CLEAR_BYTES);
    pxa_write_u64(draw + 20, 1);
    pxa_write_u16(draw + PXA_RASTER_DRAW_HEADER_BYTES + 4, 0x1111);
    pxa_esp_surface_set_raster_ready_callback(raster_ready, &raster_notifications);
    const unsigned before_submit = notifications;
    const unsigned before_raster = raster_notifications;
    assert(backend->submit(NULL, surface, draw, sizeof(draw)) == 0);
    assert(notifications == before_submit + 1 && raster_notifications == before_raster);
    pxa_esp_surface_set_raster_ready_callback(NULL, NULL);
    assert(!pxa_esp_surface_has_pending_raster_draw());
    assert(backend->submit(NULL, surface, draw, sizeof(draw)) == PXA_STATUS_BAD_STATE);
    assert(pxa_esp_surface_acquire_prepared(&first, true));
    pxa_write_u64(draw + 20, 2);
    pxa_write_u16(draw + PXA_RASTER_DRAW_HEADER_BYTES + 4, 0x2222);
    assert(backend->submit(NULL, surface, draw, sizeof(draw)) == 0);
    for (unsigned i = 0; i < 16; ++i)
        assert(((const uint16_t *)first.pixels)[i] == 0x1111);
    pxa_esp_surface_release_frame(first.lease);
    assert(pxa_esp_surface_acquire_prepared(&second, true));
    pxa_write_u64(draw + 20, 3);
    pxa_write_u16(draw + PXA_RASTER_DRAW_HEADER_BYTES + 4, 0x3333);
    assert(backend->submit(NULL, surface, draw, sizeof(draw)) == PXA_STATUS_WOULD_BLOCK);
    pxa_esp_surface_release_frame(second.lease);
    pxa_esp_surface_set_presentation_visible(false);
    assert(backend->submit(NULL, surface, draw, sizeof(draw)) == PXA_STATUS_WOULD_BLOCK);
    pxa_esp_surface_set_presentation_visible(true);
    pxa_esp_surface_set_host_visible(false);
    assert(backend->submit(NULL, surface, draw, sizeof(draw)) == PXA_STATUS_WOULD_BLOCK);
    pxa_esp_surface_set_host_visible(true);
    pxa_esp_surface_set_display_unlocked(false);
    assert(backend->submit(NULL, surface, draw, sizeof(draw)) == PXA_STATUS_WOULD_BLOCK);
    pxa_esp_surface_set_display_unlocked(true);
    draw[PXA_RASTER_DRAW_HEADER_BYTES] = 255;
    assert(backend->submit(NULL, surface, draw, sizeof(draw)) != 0);
    draw[PXA_RASTER_DRAW_HEADER_BYTES] = PXA_RASTER_RECORD_CLEAR_RGB565;
    during_raster = replace_during_raster;
    assert(backend->submit(NULL, surface, draw, sizeof(draw)) == 0);
    assert(during_raster == NULL);
    memset(draw, 0, sizeof(draw)); // The Guest may immediately reuse its memory.
    assert(pxa_esp_surface_acquire_prepared(&third, true));
    assert(third.frame_id == 3);
    for (unsigned i = 0; i < 16; ++i)
        assert(((const uint16_t *)third.pixels)[i] == 0x3333);
    pxa_esp_surface_release_frame(third.lease);
    close_surface(NULL, surface);
    assert(allocations == 0);
    assert(backend->create(NULL, &desc, &surface, &capabilities) == 0);
    pxa_write_u32(draw, PXA_RASTER_DRAW_MAGIC);
    pxa_write_u16(draw + 4, PXA_RASTER_ABI_MAJOR);
    pxa_write_u16(draw + 6, PXA_RASTER_ABI_MINOR);
    pxa_write_u32(draw + 8, sizeof(draw));
    pxa_write_u32(draw + 16, 1);
    pxa_write_u64(draw + 20, 1);
    draw[PXA_RASTER_DRAW_HEADER_BYTES] = PXA_RASTER_RECORD_CLEAR_RGB565;
    pxa_write_u16(draw + PXA_RASTER_DRAW_HEADER_BYTES + 2, PXA_RASTER_CLEAR_BYTES);
    assert(backend->upload(NULL, surface, palette, sizeof(palette)) == 0);
    during_raster = close_during_raster;
    assert(backend->submit(NULL, surface, draw, sizeof(draw)) == PXA_STATUS_CANCELLED);
    assert(during_raster == NULL && allocations == 0 && !g_materialize_active);
}

static void test_retired_display_budget(pxa_game_render_backend_t *backend) {
    pxa_game_render_desc_t desc = {4, 4, 3, 0, PXA_GAME_RENDER_SCRATCH_NONE, 4096};
    uint64_t surface;
    uint32_t capabilities;
    uint8_t palette[PXA_RASTER_UPLOAD_HEADER_BYTES + 512] = {0};
    uint8_t draw[PXA_RASTER_DRAW_HEADER_BYTES + PXA_RASTER_CLEAR_BYTES] = {0};
    pxa_esp_surface_frame_t frame;
    assert(backend->create(NULL, &desc, &surface, &capabilities) == 0);
    snapshot_upload_header(palette, PXA_RASTER_UPLOAD_PALETTE_RGB565, 256, 1);
    assert(backend->upload(NULL, surface, palette, sizeof(palette)) == 0);
    pxa_write_u32(draw, PXA_RASTER_DRAW_MAGIC);
    pxa_write_u16(draw + 4, PXA_RASTER_ABI_MAJOR);
    pxa_write_u16(draw + 6, PXA_RASTER_ABI_MINOR);
    pxa_write_u32(draw + 8, sizeof(draw));
    pxa_write_u32(draw + 16, 1);
    pxa_write_u64(draw + 20, 1);
    draw[PXA_RASTER_DRAW_HEADER_BYTES] = PXA_RASTER_RECORD_CLEAR_RGB565;
    pxa_write_u16(draw + PXA_RASTER_DRAW_HEADER_BYTES + 2, PXA_RASTER_CLEAR_BYTES);
    assert(backend->submit(NULL, surface, draw, sizeof(draw)) == 0);
    assert(pxa_esp_surface_acquire_latest_for_direct(&frame));
    backend->close(NULL, surface);
    assert(allocations > 0); /* Display still owns its scanout lease. */
    assert(pxa_esp_resource_memory_end() == PXA_STATUS_WOULD_BLOCK);
    assert(pxa_esp_resource_memory_begin() == PXA_STATUS_OK);
    assert(!backend->create(NULL, &desc, &surface, &capabilities));
    assert(!backend->upload(NULL, surface, palette, sizeof(palette)));
    pxa_esp_surface_release_frame(frame.lease); /* Late old frame releases old owner. */
    backend->close(NULL, surface);
    assert(allocations == 0);
    pxa_esp_surface_memory_info_t memory = {0};
    pxa_esp_surface_memory_snapshot(&memory);
    assert(memory.frame_bytes == 0 && memory.scratch_bytes == 0 &&
           memory.mailbox_bytes == 0);
}

static void fill_test_pixels(void *context, uint8_t *pixels, uint32_t stride, uint8_t index) {
    (void)index;
    memset(pixels, *(const uint8_t *)context, stride * 4);
}

static unsigned native_draws;
static bool draw_native_color(const void *payload, const pxa_esp_surface_native_target_t *dst) {
    assert(!lock_depth && dst->width == 4 && dst->height == 4 && dst->size == 128 &&
           dst->stride_bytes == 8 && dst->rotation == 0);
    ++native_draws;
    memset(dst->pixels, *(const uint8_t *)payload, dst->size);
    return true;
}

static void fill_native_color(void *context, uint8_t *pixels, uint32_t stride, uint8_t index) {
    void *payload = NULL;
    assert(((uintptr_t)pixels & 127u) == 0);
    assert(pxa_esp_surface_staging_capacity(pixels, 32) == 128);
    assert(!pxa_esp_surface_staging_capacity(pixels, 33));
    assert(!pxa_esp_surface_staging_capacity(pixels + 2, 32));
    assert(pxa_esp_surface_stage_native(pixels, 16, draw_native_color, &payload));
    assert(((uintptr_t)payload & 127u) == 0);
    *(uint8_t *)payload = *(const uint8_t *)context;
    if (!*(uint8_t *)payload) {
        pxa_esp_surface_cancel_native(pixels);
        fill_test_pixels(context, pixels, stride, index);
    }
}

typedef struct {
    unsigned frame;
    size_t bytes;
    bool cancel;
    bool inlined;
    uint8_t *base;
} inline_case_t;

static unsigned inline_draws;
static bool fail_after_draw;
static bool draw_inline_test(const void *payload, const pxa_esp_surface_native_target_t *dst) {
    assert(payload != dst->pixels && ((uintptr_t)dst->pixels & 127u) == 0);
    memset(dst->pixels, *(const uint8_t *)payload, dst->size);
    ++inline_draws;
    if (fail_after_draw) {
        memset(dst->pixels, 0xce, dst->size);
        fail_after_draw = false;
        return false;
    }
    return true;
}
static void fill_inline_test(void *ctx, uint8_t *pixels, uint32_t stride, uint8_t index) {
    (void)index;
    inline_case_t *test = ctx;
    void *payload = NULL;
    test->base = pixels;
    assert(pxa_esp_surface_stage_native(pixels, test->bytes, draw_inline_test, &payload));
    test->inlined = pixels == payload;
    memset(payload, test->frame, test->bytes);
    if (test->cancel) {
        pxa_esp_surface_cancel_native(pixels);
        memset(pixels + stride + 2, 0x78, 4);
        memset(pixels + 2 * stride + 2, 0x78, 4);
    }
}
static void partial_test(void *ctx, uint8_t *pixels, uint32_t stride, uint8_t index) {
    (void)index;
    (void)stride;
    assert(pixels[30] == *(uint8_t *)ctx);
    pixels[0] = 0x9a;
}
static void check_inline_slots(pxa_surface_backend_t *backend) {
    const pxa_surface_desc_t desc = {4, 4, PXA_SURFACE_FORMAT_RGB565, 2,
                                     PXA_SURFACE_FLAG_HOST_PRODUCED};
    const pxa_surface_layer_t layer = {.width = 4, .height = 4, .visible = 1};
    uint64_t handle;
    uint32_t stride;
    pxa_esp_surface_frame_t a, b;
    assert(!backend->create(NULL, &desc, &handle, &stride) &&
           !backend->configure(NULL, handle, &layer));
    pxa_esp_surface_t *surface = (void *)(uintptr_t)handle;
    inline_case_t test = {.frame = 0x12, .bytes = 32};
    pxa_surface_region_fill_t region = {fill_inline_test, &test, {1, 1, 2, 2}, NULL};
    assert(!backend->stage(NULL, handle, pxa_surface_fill_region, &region) && test.inlined);
    assert(!backend->queue_ordered(NULL, handle, 1, NULL, 0));
    assert(pxa_esp_surface_acquire_latest_native_for_direct(&a) && a.native_payload == test.base &&
           !a.pixels);
    const uint8_t *initial = a.native_payload;
    test.frame = 0x34;
    assert(!backend->stage(NULL, handle, pxa_surface_fill_region, &region) && test.inlined &&
           initial != test.base);
    assert(!backend->queue_ordered(NULL, handle, 2, NULL, 0));
    assert(initial[0] == 0x12 && initial[31] == 0x12);
    assert(!surface->native[0].storage && !surface->native[1].storage);
    fail_readback = true;
    assert(!pxa_esp_surface_materialize_frame(&a) && !a.pixels && initial[0] == 0x12);
    assert(pxa_esp_surface_materialize_frame(&a) && a.pixels != initial && a.pixels[0] == 0x12);
    assert(pxa_esp_surface_materialize_frame(&a) && inline_draws == 1 && initial[31] == 0x12);
    pxa_esp_surface_release_frame(a.lease);
    assert(pxa_esp_surface_acquire_latest_native_for_direct(&b) && !b.pixels);
    pxa_esp_surface_release_frame(b.lease);
    test.frame = 0xad;
    test.cancel = true;
    assert(!backend->stage(NULL, handle, pxa_surface_fill_region, &region) && test.inlined);
    assert(!backend->queue_ordered(NULL, handle, 3, NULL, 0));
    assert(pxa_esp_surface_acquire_latest(&a) && !a.native_draw);
    for (unsigned y = 0; y < 4; ++y)
        for (unsigned x = 0; x < 4; ++x)
            assert(a.pixels[y * 8 + x * 2] ==
                   ((x == 1 || x == 2) && (y == 1 || y == 2) ? 0x78 : 0));
    pxa_esp_surface_release_frame(a.lease);
    /* The free slot now contains frame 2. A legacy partial writer must see
     * its prior RGB, even without a previous readback. */
    uint8_t expected = 0x34;
    assert(!backend->stage(NULL, handle, partial_test, &expected));
    assert(!backend->queue_ordered(NULL, handle, 4, NULL, 0));
    assert(pxa_esp_surface_acquire_latest(&a) && a.pixels[0] == 0x9a && a.pixels[10] == 0x34);
    pxa_esp_surface_release_frame(a.lease);
    test.cancel = false;
    test.frame = 0x55;
    test.bytes = 256;
    assert(!backend->stage(NULL, handle, pxa_surface_fill_region, &region) && !test.inlined);
    assert(!backend->queue_ordered(NULL, handle, 5, NULL, 0));
    assert(pxa_esp_surface_acquire_latest(&b) && b.pixels[0] == 0x55);
    pxa_esp_surface_release_frame(b.lease);
    test.frame = 0x65;
    test.bytes = 32;
    assert(!backend->stage(NULL, handle, pxa_surface_fill_region, &region) && test.inlined);
    assert(!backend->queue_ordered(NULL, handle, 6, NULL, 0));
    assert(pxa_esp_surface_acquire_latest_native_for_direct(&a) && a.native_payload == test.base);
    initial = a.native_payload;
    assert(pxa_esp_surface_materialize_frame(&a) && a.pixels[0] == 0x65);
    unsigned n = inline_draws;
    pxa_esp_surface_release_frame(a.lease);
    assert(pxa_esp_surface_acquire_current_for_preview(&a) && a.pixels[31] == 0x65 &&
           n == inline_draws);
    pxa_esp_surface_release_frame(a.lease);
    assert(pxa_esp_surface_acquire_prepared_native(&a) && a.native_payload == initial);
    backend->close(NULL, handle);
    assert(pxa_esp_surface_materialize_frame(&a) && a.pixels[0] == 0x65 && initial[31] == 0x65);
    pxa_esp_surface_release_frame(a.lease);
}

static void check_single_readback(pxa_surface_backend_t *backend) {
    const pxa_surface_desc_t desc = {4, 4, PXA_SURFACE_FORMAT_RGB565, 2,
                                     PXA_SURFACE_FLAG_HOST_PRODUCED};
    const pxa_surface_layer_t layer = {.width = 4, .height = 4, .visible = 1};
    uint64_t handle;
    uint32_t stride;
    pxa_esp_surface_frame_t frame;
    assert(!backend->create(NULL, &desc, &handle, &stride) &&
           !backend->configure(NULL, handle, &layer));
    pxa_esp_surface_t *surface = (void *)(uintptr_t)handle;
    inline_case_t test = {.frame = 0x11, .bytes = 32};
    pxa_surface_region_fill_t region = {fill_inline_test, &test, {0, 0, 4, 4}, NULL};
    assert(!backend->stage(NULL, handle, pxa_surface_fill_region, &region));
    assert(!backend->queue_ordered(NULL, handle, 1, NULL, 0));
    assert(pxa_esp_surface_acquire_latest_native_for_direct(&frame));
    pxa_esp_surface_release_frame(frame.lease);
    test.frame = 0x33;
    assert(!backend->stage(NULL, handle, pxa_surface_fill_region, &region));
    assert(!backend->queue_ordered(NULL, handle, 2, NULL, 0));
    assert(pxa_esp_surface_acquire_latest(&frame) && frame.pixels == surface->readback &&
           frame.pixels[31] == 0x33);
    uint8_t *shared = surface->readback;
    uint8_t old = 0x11;
    fail_after_draw = true;
    assert(backend->stage(NULL, handle, partial_test, &old) == PXA_STATUS_IO_ERROR);
    assert(frame.pixels[12] == 0x33);
    assert(!backend->stage(NULL, handle, partial_test, &old));
    assert(frame.pixels[10] == 0x33 && frame.pixels == shared);
    pxa_esp_surface_release_frame(frame.lease);
    assert(!backend->queue_ordered(NULL, handle, 3, NULL, 0));
    assert(pxa_esp_surface_acquire_latest(&frame) && frame.pixels[0] == 0x9a);
    pxa_esp_surface_release_frame(frame.lease);
    for (unsigned id = 4; id < 16; ++id) {
        test.frame = id;
        assert(!backend->stage(NULL, handle, pxa_surface_fill_region, &region));
        assert(!backend->queue_ordered(NULL, handle, id, NULL, 0));
        assert(pxa_esp_surface_acquire_latest(&frame) && frame.pixels == shared &&
               frame.pixels[31] == id);
        pxa_esp_surface_release_frame(frame.lease);
    }
    for (unsigned id = 16; id < 18; ++id) {
        test.frame = id;
        assert(!backend->stage(NULL, handle, pxa_surface_fill_region, &region));
        assert(!backend->queue_ordered(NULL, handle, id, NULL, 0));
        assert(pxa_esp_surface_acquire_latest_native_for_direct(&frame) && !frame.pixels &&
               frame.native_payload);
        pxa_esp_surface_release_frame(frame.lease);
    }
    assert(!surface->readback);
    backend->close(NULL, handle);
}

static void check_ordered_producer(pxa_surface_backend_t *backend) {
    const pxa_surface_desc_t desc = {4, 4, PXA_SURFACE_FORMAT_RGB565, 3, PXA_SURFACE_FLAG_HOST_PRODUCED};
    const pxa_surface_layer_t layer = {.width = 4, .height = 4, .visible = 1};
    uint64_t surface;
    uint32_t stride;
    uint8_t color = 1;
    pxa_esp_surface_frame_t reading;
    pxa_surface_state_t state;
    assert(!backend->create(NULL, &desc, &surface, &stride));
    assert(!backend->configure(NULL, surface, &layer));
    assert(!backend->stage(NULL, surface, fill_test_pixels, &color));
    assert(!backend->queue_ordered(NULL, surface, 1, NULL, 0));
    assert(pxa_esp_surface_acquire_latest(&reading) && reading.pixels[0] == 1);
    color = 2;
    assert(!backend->stage(NULL, surface, fill_test_pixels, &color));
    assert(!backend->queue_ordered(NULL, surface, 2, NULL, 0));
    color = 3;
    assert(!backend->stage(NULL, surface, fill_test_pixels, &color));
    assert(backend->queue_ordered(NULL, surface, 3, NULL, 0) == PXA_STATUS_WOULD_BLOCK);
    assert(backend->queue_ordered(NULL, surface, 3, NULL, 0) == PXA_STATUS_WOULD_BLOCK);
    assert(!backend->query(NULL, surface, &state));
    assert(state.submitted_frames == 2 && !state.dropped_frames && !state.replaced_frames);
    assert(reading.frame_id == 1 && reading.pixels[0] == 1);
    pxa_esp_surface_release_frame(reading.lease);
    assert(pxa_esp_surface_acquire_latest(&reading) && reading.frame_id == 2 && reading.pixels[0] == 2);
    assert(!backend->queue_ordered(NULL, surface, 3, NULL, 0));
    assert(backend->queue_ordered(NULL, surface, 3, NULL, 0) == PXA_STATUS_BAD_STATE);
    pxa_esp_surface_release_frame(reading.lease);
    assert(pxa_esp_surface_acquire_latest(&reading) && reading.frame_id == 3 && reading.pixels[0] == 3);
    backend->close(NULL, surface);
    assert(reading.pixels[0] == 3);
    pxa_esp_surface_release_frame(reading.lease);
}

static void check_native_producer(pxa_surface_backend_t *backend) {
    const pxa_surface_desc_t desc = {4, 4, PXA_SURFACE_FORMAT_RGB565, 2, PXA_SURFACE_FLAG_HOST_PRODUCED};
    const pxa_surface_layer_t layer = {.width = 4, .height = 4, .visible = 1};
    uint64_t surface;
    uint32_t stride;
    uint8_t color = 21;
    pxa_esp_surface_frame_t reading;
    assert(!backend->create(NULL, &desc, &surface, &stride));
    assert(!backend->configure(NULL, surface, &layer));
    /* Replace an unpresented native slot without doing RGB work. */
    assert(!backend->stage(NULL, surface, fill_native_color, &color));
    assert(!backend->queue(NULL, surface, 1, NULL, 0));
    ++color;
    assert(!backend->stage(NULL, surface, fill_native_color, &color));
    assert(!backend->queue(NULL, surface, 2, NULL, 0));
    assert(pxa_esp_surface_acquire_latest_native_for_direct(&reading));
    assert(!native_draws && reading.native_draw == draw_native_color && !reading.pixels &&
           *(const uint8_t *)reading.native_payload == 22);
    void *payload;
    assert(!pxa_esp_surface_stage_native(NULL, 1, draw_native_color, &payload) && !payload);
    assert(pxa_esp_surface_materialize_frame(&reading) && reading.pixels[0] == 22);
    assert(pxa_esp_surface_materialize_frame(&reading) && native_draws == 1);
    pxa_esp_surface_release_frame(reading.lease);
    assert(!pxa_esp_surface_materialize_frame(&reading));
    assert(pxa_esp_surface_acquire_current_for_preview(&reading) && reading.pixels[0] == 22);
    assert(native_draws == 1 && !reading.native_draw);
    pxa_esp_surface_release_frame(reading.lease);
    color = 0;
    assert(!backend->stage(NULL, surface, fill_native_color, &color));
    assert(!backend->queue(NULL, surface, 3, NULL, 0));
    assert(pxa_esp_surface_acquire_prepared_native(&reading) && !reading.native_draw &&
           reading.pixels[0] == 0);
    pxa_esp_surface_release_frame(reading.lease);
    color = 31;
    assert(!backend->stage(NULL, surface, fill_native_color, &color));
    assert(!backend->queue(NULL, surface, 4, NULL, 0));
    assert(pxa_esp_surface_acquire_latest(&reading) && reading.pixels[0] == 31 && native_draws == 2);
    pxa_esp_surface_release_frame(reading.lease);
    color = 32;
    assert(!backend->stage(NULL, surface, fill_native_color, &color));
    assert(!backend->queue(NULL, surface, 5, NULL, 0));
    assert(pxa_esp_surface_acquire_prepared_native(&reading));
    backend->close(NULL, surface);
    assert(pxa_esp_surface_materialize_frame(&reading) && reading.pixels[0] == 32);
    pxa_esp_surface_release_frame(reading.lease);
}

static void check_producer_front_buffer(pxa_surface_backend_t *backend) {
    const pxa_surface_desc_t desc = {4, 4, PXA_SURFACE_FORMAT_RGB565, 2, PXA_SURFACE_FLAG_HOST_PRODUCED};
    const pxa_surface_layer_t layer = {.width = 4, .height = 4, .visible = 1};
    uint64_t surface;
    uint32_t stride;
    uint8_t color = 1;
    pxa_esp_surface_frame_t reading;
    assert(backend->create(backend->context, &desc, &surface, &stride) == PXA_STATUS_OK);
    assert(backend->configure(backend->context, surface, &layer) == PXA_STATUS_OK);
    assert(backend->stage(backend->context, surface, fill_test_pixels, &color) == PXA_STATUS_OK);
    assert(backend->queue(backend->context, surface, 1, NULL, 0) == PXA_STATUS_OK);
    assert(pxa_esp_surface_acquire_latest(&reading) && reading.pixels[0] == 1);
    pxa_esp_surface_release_frame(reading.lease);
    ++color;
    assert(backend->stage(backend->context, surface, fill_test_pixels, &color) == PXA_STATUS_OK);
    assert(backend->queue(backend->context, surface, 2, NULL, 0) == PXA_STATUS_OK);
    assert(pxa_esp_surface_acquire_current_for_preview(&reading) && reading.pixels[0] == 1);
    ++color;
    assert(backend->stage(backend->context, surface, fill_test_pixels, &color) == PXA_STATUS_WOULD_BLOCK);
    pxa_esp_surface_release_frame(reading.lease);
    assert(pxa_esp_surface_acquire_latest(&reading) && reading.pixels[0] == 2);
    assert(backend->stage(backend->context, surface, fill_test_pixels, &color) == PXA_STATUS_OK);
    assert(reading.pixels[0] == 2);
    assert(backend->queue(backend->context, surface, 3, NULL, 0) == PXA_STATUS_OK);
    pxa_esp_surface_release_frame(reading.lease);
    assert(pxa_esp_surface_acquire_current_for_preview(&reading) && reading.pixels[0] == 2);
    pxa_esp_surface_release_frame(reading.lease);
    assert(pxa_esp_surface_acquire_latest(&reading) && reading.pixels[0] == 3);
    pxa_esp_surface_release_frame(reading.lease);
    backend->close(backend->context, surface);
}

int main(void) {
    assert(pxa_esp_resource_memory_begin() == PXA_STATUS_OK);
    {
        const raster_allocator_t policy = {
            pxa_esp_resource_allocator(PXA_MEMORY_INTERNAL, PXA_MEMORY_RASTER),
            pxa_esp_resource_allocator(PXA_MEMORY_EXTERNAL, PXA_MEMORY_RASTER)};
        void *small = allocate_raster_resource((void *)&policy, 256);
        assert(small && (last_malloc_caps & MALLOC_CAP_INTERNAL));
        free_raster_resource(NULL, small);
        void *large = allocate_raster_resource((void *)&policy, 16408);
#if CONFIG_PXA_RASTER_INTERNAL_ALLOCATION_MAX_BYTES > 0
        assert(large && (last_malloc_caps & MALLOC_CAP_SPIRAM));
#else
        assert(large && (last_malloc_caps & MALLOC_CAP_INTERNAL));
#endif
        free_raster_resource(NULL, large);
        assert(allocations == 0);
    }
    pxa_surface_backend_t backend;
    pxa_game_render_backend_t game_backend;
    pxa_surface_desc_t desc = {
        4, 4, PXA_SURFACE_FORMAT_RGB565, 3,
        PXA_SURFACE_FLAG_PREFER_DIRECT_SCANOUT};
    pxa_surface_layer_t layer = {2, 3, 4, 4, 0, 1, 0};
    pxa_surface_damage_rect_t overlay = {1, 1, 2, 2};
    pxa_surface_state_t state;
    pxa_esp_surface_input_metrics_t input_metrics;
    pxa_esp_surface_memory_info_t memory_info;
    pxa_esp_surface_frame_t frame;
    pxa_esp_surface_frame_t probe;
    pxa_esp_surface_present_info_t present_info;
    uint8_t first[32];
    uint8_t second[32];
    uint8_t third[32];
    uint8_t alpha_pixels[64];
    uint64_t surface;
    uint64_t lease;
    uint32_t stride;
    static uint8_t mapped_storage[3 * 32 + PXA_SURFACE_BUFFER_ALIGNMENT]
        __attribute__((aligned(PXA_SURFACE_BUFFER_ALIGNMENT)));
    uint8_t *mapped_pixels = mapped_storage + 2;
    pxa_surface_release_t released;
    uint8_t buffer_index;
    memset(first, 0x11, sizeof(first));
    memset(second, 0x22, sizeof(second));
    memset(third, 0x33, sizeof(third));
    memset(alpha_pixels, 0x44, sizeof(alpha_pixels));
    pxa_esp_surface_set_frame_ready_callback(ready, &notifications);
    pxa_esp_surface_set_release_ready_callback(release_ready,
                                                &release_notifications);
    pxa_esp_surface_backend(&backend);
    pxa_esp_game_render_backend(&game_backend);
    assert(backend.create(backend.context, &desc, &surface, &stride) ==
           PXA_STATUS_OK);
    assert(surface != 0 && stride == 8 && allocations == 4);
    pxa_esp_surface_memory_snapshot(&memory_info);
    assert(memory_info.frame_bytes >= 3u * sizeof(first) &&
           memory_info.scratch_bytes == 0 && memory_info.mailbox_bytes == 0 &&
           memory_info.probe_bytes == 0);
    assert(backend.configure(backend.context, surface, &layer) ==
           PXA_STATUS_OK && notifications == 1);
    assert(pxa_esp_surface_get_present_info(&present_info) &&
           present_info.width == 4 && present_info.height == 4 &&
           present_info.x == 2 && present_info.y == 3 &&
           present_info.visible == 1);
    assert(backend.configure_opaque_ui_regions(backend.context, surface, &overlay, 1) ==
           PXA_STATUS_OK && notifications == 2);

    assert(backend.write(backend.context, surface, first, sizeof(first)) ==
           PXA_STATUS_OK);
    assert(backend.queue(backend.context, surface, 1, NULL, 0) ==
           PXA_STATUS_OK);
    assert(backend.write(backend.context, surface, second, sizeof(second)) ==
           PXA_STATUS_OK);
    assert(backend.queue(backend.context, surface, 2, NULL, 0) ==
           PXA_STATUS_OK);
    assert(backend.query(backend.context, surface, &state) == PXA_STATUS_OK &&
           state.submitted_frames == 2 && state.dropped_frames == 1 &&
           state.presented_frames == 0 && state.free_buffers == 2);
    assert(pxa_esp_surface_has_pending_frame());
    pxa_esp_surface_set_host_visible(false);
    assert(notifications == 5);
    assert(pxa_esp_surface_get_present_info(&present_info) &&
           present_info.visible == 0);
    assert(!pxa_esp_surface_has_pending_frame());
    assert(!pxa_esp_surface_acquire_latest(&probe));
    pxa_esp_surface_set_host_visible(true);
    assert(notifications == 6);
    assert(pxa_esp_surface_get_present_info(&present_info) &&
           present_info.visible == 1);
    assert(pxa_esp_surface_has_pending_frame());
    assert(!pxa_esp_surface_composition_required());
    pxa_esp_surface_set_system_overlay_visible(true);
    assert(notifications == 7 && pxa_esp_surface_composition_required());
    assert(!pxa_esp_surface_acquire_latest_native_for_direct(&probe));
    assert(!pxa_esp_surface_acquire_latest_for_direct(&probe));
    assert(pxa_esp_surface_acquire_latest(&probe));
    assert(probe.opaque_ui_region_count == 1 &&
           probe.opaque_ui_regions[0].x == 0 &&
           probe.opaque_ui_regions[0].y == 0 &&
           probe.opaque_ui_regions[0].width == 4 &&
           probe.opaque_ui_regions[0].height == 4);
    pxa_esp_surface_release_frame(probe.lease);
    pxa_esp_surface_set_system_overlay_visible(false);
    assert(notifications == 8 && !pxa_esp_surface_composition_required());
    pxa_esp_surface_runtime_modal_enter();
    assert(notifications == 9 && pxa_esp_surface_composition_required());
    pxa_esp_surface_runtime_modal_enter();
    assert(notifications == 9 && pxa_esp_surface_composition_required());
    pxa_esp_surface_runtime_modal_leave();
    assert(notifications == 9 && pxa_esp_surface_composition_required());
    pxa_esp_surface_runtime_modal_leave();
    assert(notifications == 10 && !pxa_esp_surface_composition_required());
    assert(!pxa_esp_surface_acquire_latest_native_for_direct(&probe));
    assert(!pxa_esp_surface_acquire_latest_for_direct(&probe));
    assert(!pxa_esp_surface_try_resume_direct_scanout(2));
    assert(!pxa_esp_surface_try_resume_direct_scanout(1));
    pxa_esp_surface_require_composition();
    assert(notifications == 11 && pxa_esp_surface_composition_required());
    g_ui_alpha_provider = empty_ui_alpha_plane;
    g_ui_alpha_provider_context = &notifications;
    assert(!pxa_esp_surface_composition_required());
    g_ui_alpha_provider = NULL;
    g_ui_alpha_provider_context = NULL;
    assert(pxa_esp_surface_composition_required());

    assert(pxa_esp_surface_acquire_latest(&frame));
    assert(frame.frame_id == 2 && frame.x == 2 && frame.y == 3 &&
           frame.width == 4 && frame.height == 4 && frame.visible &&
           frame.opaque_ui_region_count == 1 &&
           frame.opaque_ui_regions[0].x == 1 &&
           memcmp(frame.pixels, second, sizeof(second)) == 0);
    assert(!pxa_esp_surface_has_pending_frame());
    lease = frame.lease;
    assert(!pxa_esp_surface_acquire_latest(&probe));
    assert(backend.write(backend.context, surface, third, sizeof(third)) ==
           PXA_STATUS_OK);
    assert(backend.queue(backend.context, surface, 3, NULL, 0) ==
           PXA_STATUS_OK);
    assert(pxa_esp_surface_has_pending_frame());
    pxa_esp_surface_release_frame(lease);
    assert(((pxa_esp_surface_t *)(uintptr_t)surface)
               ->current_frame_id == 2);
    assert(pxa_esp_surface_acquire_current_for_preview(&probe) &&
           probe.frame_id == 2 &&
           memcmp(probe.pixels, second, sizeof(second)) == 0);
    assert(pxa_esp_surface_has_pending_frame());
    pxa_esp_surface_release_frame(probe.lease);
    assert(pxa_esp_surface_has_pending_frame());
    assert(backend.query(backend.context, surface, &state) == PXA_STATUS_OK &&
           state.presented_frames == 1 && state.free_buffers == 1);

    assert(pxa_esp_surface_acquire_latest(&frame) && frame.frame_id == 3 &&
           memcmp(frame.pixels, third, sizeof(third)) == 0);
    assert(pxa_esp_surface_try_resume_direct_scanout(frame.frame_id));
    assert(pxa_esp_surface_try_resume_direct_scanout(frame.frame_id));
    assert(!pxa_esp_surface_has_pending_frame());
    pxa_esp_surface_release_frame(frame.lease);
    assert(backend.query(backend.context, surface, &state) == PXA_STATUS_OK &&
           state.presented_frames == 2 && state.free_buffers == 2);

    assert(pxa_esp_surface_acquire_latest(&frame) && frame.frame_id == 3);
    lease = frame.lease;
    backend.close(backend.context, surface);
    assert(allocations == 4 && !pxa_esp_surface_acquire_latest(&probe));
    pxa_esp_surface_release_frame(lease);
    assert(allocations == 0 && notifications == 13 && lock_depth == 0);
    pxa_esp_surface_memory_snapshot(&memory_info);
    assert(memory_info.frame_bytes == 0 && memory_info.scratch_bytes == 0 &&
           memory_info.mailbox_bytes == 0);

    g_ui_alpha_provider = empty_ui_alpha_plane;
    g_ui_alpha_provider_context = &notifications;
    assert(!pxa_esp_surface_composition_required());
    pxa_esp_surface_set_power_overlay_visible(true);
    pxa_esp_surface_set_system_overlay_visible(true);
    pxa_esp_surface_set_power_overlay_visible(false);
    assert(pxa_esp_surface_composition_required());
    pxa_esp_surface_set_system_overlay_visible(false);
    assert(!pxa_esp_surface_composition_required());
    g_ui_alpha_provider = NULL;
    g_ui_alpha_provider_context = NULL;

    pxa_esp_surface_set_host_visible(false);
    pxa_esp_surface_set_display_unlocked(false);
    pxa_esp_surface_set_host_visible(true);
    desc.format = PXA_SURFACE_FORMAT_ARGB8888_PREMULTIPLIED;
    desc.flags = PXA_SURFACE_FLAG_PREMULTIPLIED_ALPHA;
    desc.buffer_count = 2;
    pxa_esp_surface_set_ui_alpha_provider(ui_alpha_plane, &notifications);
    assert(backend.create(backend.context, &desc, &surface, &stride) ==
           PXA_STATUS_OK);
    assert(stride == 16 && allocations == 3);
    assert(!pxa_esp_surface_composition_required());
    assert(backend.configure(backend.context, surface, &layer) ==
           PXA_STATUS_OK);
    assert(pxa_esp_surface_get_present_info(&present_info) &&
           !present_info.visible && !pxa_esp_surface_acquire_latest(&probe));
    pxa_esp_surface_set_display_unlocked(true);
    assert(pxa_esp_surface_get_present_info(&present_info) &&
           present_info.visible);
    assert(backend.write(backend.context, surface, alpha_pixels,
                         sizeof(alpha_pixels)) == PXA_STATUS_OK);
    assert(backend.queue(backend.context, surface, 4, NULL, 0) ==
           PXA_STATUS_OK);
    assert(pxa_esp_surface_acquire_latest(&frame));
    assert(frame.format == PXA_SURFACE_FORMAT_ARGB8888_PREMULTIPLIED &&
           frame.flags == PXA_SURFACE_FLAG_PREMULTIPLIED_ALPHA &&
           frame.stride_bytes == 16 &&
           frame.ui_alpha_plane.pixels != NULL &&
           frame.ui_alpha_plane.alpha != NULL &&
           memcmp(frame.pixels, alpha_pixels, sizeof(alpha_pixels)) == 0);
    pxa_esp_surface_release_frame(frame.lease);
    assert(backend.query(backend.context, surface, &state) == PXA_STATUS_OK &&
           (state.flags & PXA_SURFACE_STATE_FLAG_SUPPORTS_ALPHA_COMPOSITING) != 0 &&
           (state.flags & PXA_SURFACE_STATE_FLAG_UI_ALPHA_PLANE_ACTIVE) != 0);
    backend.close(backend.context, surface);
    assert(allocations == 0);
    pxa_esp_surface_set_ui_alpha_provider(NULL, NULL);

    desc.format = PXA_SURFACE_FORMAT_RGB565;
    desc.flags = PXA_SURFACE_FLAG_PREFER_DIRECT_SCANOUT |
                 PXA_SURFACE_FLAG_GUEST_MAPPED;
    desc.buffer_count = 3;
    assert(backend.create(backend.context, &desc, &surface, &stride) ==
           PXA_STATUS_OK);
    assert(stride == 8 && allocations == 1);
    assert(backend.configure(backend.context, surface, &layer) ==
           PXA_STATUS_OK);
    assert(backend.acquire_buffer(backend.context, surface, &buffer_index) ==
           PXA_STATUS_BAD_STATE);
    assert(backend.register_buffers(backend.context, surface,
                                    mapped_pixels + 1,
                                    3 * 32) ==
           PXA_STATUS_INVALID_ARGUMENT);
    assert(backend.register_buffers(backend.context, surface,
                                    mapped_pixels,
                                    3 * 32) == PXA_STATUS_OK);
    assert(backend.register_buffers(backend.context, surface,
                                    mapped_pixels,
                                    3 * 32) ==
           PXA_STATUS_BAD_STATE);

    assert(backend.acquire_buffer(backend.context, surface, &buffer_index) ==
               PXA_STATUS_OK &&
           buffer_index == 0);
    assert(backend.acquire_buffer(backend.context, surface, &buffer_index) ==
           PXA_STATUS_BAD_STATE);
    assert(backend.present_buffer(backend.context, surface, 1, 1) ==
           PXA_STATUS_BAD_STATE);
    assert(backend.present_buffer(backend.context, surface, 0, 1) ==
           PXA_STATUS_OK);
    assert(backend.acquire_buffer(backend.context, surface, &buffer_index) ==
               PXA_STATUS_OK &&
           buffer_index == 1);
    assert(backend.present_buffer(backend.context, surface, 1, 2) ==
           PXA_STATUS_OK);
    assert(release_notifications == 1);
    assert(backend.peek_release(backend.context, surface, &released) ==
               PXA_STATUS_OK &&
           released.buffer_index == 0 && released.frame_id == 1);
    assert(backend.query(backend.context, surface, &state) == PXA_STATUS_OK &&
           state.submitted_frames == 2 && state.dropped_frames == 1 &&
           state.replaced_frames == 1 && state.released_frames == 0 &&
           state.free_buffers == 1 &&
           (state.flags & PXA_SURFACE_STATE_FLAG_SUPPORTS_GUEST_MAPPED) != 0);
    backend.consume_release(backend.context, surface);
    assert(backend.query(backend.context, surface, &state) == PXA_STATUS_OK &&
           state.released_frames == 1 && state.free_buffers == 2);

    assert(pxa_esp_surface_acquire_latest(&frame) && frame.frame_id == 2 &&
           frame.pixels == mapped_pixels + 32);
    assert(backend.acquire_buffer(backend.context, surface, &buffer_index) ==
               PXA_STATUS_OK &&
           buffer_index == 0);
    assert(backend.present_buffer(backend.context, surface, 0, 2) ==
           PXA_STATUS_BAD_STATE);
    pxa_esp_surface_note_input_sample(1000);
    pxa_esp_surface_note_input_delivered(1000, 1450);
    test_now_us = 1600;
    assert(backend.present_buffer(backend.context, surface, 0, 3) ==
           PXA_STATUS_OK);
    assert(backend.acquire_buffer(backend.context, surface, &buffer_index) ==
               PXA_STATUS_OK &&
           buffer_index == 2);
    test_now_us = 1800;
    assert(backend.present_buffer(backend.context, surface, 2, 4) ==
           PXA_STATUS_OK);
    pxa_esp_surface_release_frame(frame.lease);
    assert(release_notifications == 2);
    assert(backend.peek_release(backend.context, surface, &released) ==
               PXA_STATUS_OK &&
           released.buffer_index == 0 && released.frame_id == 3);
    backend.consume_release(backend.context, surface);
    assert(pxa_esp_surface_acquire_latest_for_direct(&frame) &&
           frame.frame_id == 4 &&
           frame.input_timestamp_us == 1000 &&
           frame.pixels == mapped_pixels + 64);
    assert(release_notifications == 3);
    assert(backend.peek_release(backend.context, surface, &released) ==
               PXA_STATUS_OK &&
           released.buffer_index == 1 && released.frame_id == 2);
    backend.consume_release(backend.context, surface);
    pxa_esp_surface_note_frame_presented(frame.input_timestamp_us, 2200);
    pxa_esp_surface_take_display_input_metrics(&input_metrics);
    assert(input_metrics.sample_to_guest_count == 1 &&
           input_metrics.sample_to_guest_total_us == 450 &&
           input_metrics.sample_to_guest_max_us == 450 &&
           input_metrics.sample_to_guest_p95_us == 1000 &&
           input_metrics.sample_to_present_count == 1 &&
           input_metrics.sample_to_present_total_us == 600 &&
           input_metrics.sample_to_present_max_us == 600 &&
           input_metrics.sample_to_present_p95_us == 1000 &&
           input_metrics.sample_to_visible_count == 1 &&
           input_metrics.sample_to_visible_total_us == 1200 &&
           input_metrics.sample_to_visible_max_us == 1200 &&
           input_metrics.sample_to_visible_p95_us == 2000);
    pxa_esp_surface_take_input_metrics(&input_metrics);
    assert(input_metrics.sample_to_guest_count == 0 &&
           input_metrics.sample_to_present_count == 0 &&
           input_metrics.sample_to_visible_count == 0);
    for (unsigned sample = 0; sample < 100; ++sample) {
        const uint64_t elapsed = sample < 95 ? 10000u : 80000u;
        pxa_esp_surface_note_input_delivered(1000, 1000 + elapsed);
        pxa_esp_surface_note_frame_presented(1000, 1000 + elapsed);
    }
    pxa_esp_surface_take_input_metrics(&input_metrics);
    assert(input_metrics.sample_to_guest_count == 100 &&
           input_metrics.sample_to_guest_p95_us == 11000 &&
           input_metrics.sample_to_guest_max_us == 80000 &&
           input_metrics.sample_to_visible_count == 100 &&
           input_metrics.sample_to_visible_p95_us == 11000 &&
           input_metrics.sample_to_visible_max_us == 80000);
    assert(pxa_esp_surface_perf_start());
    pxa_esp_surface_note_input_delivered(1000, 1600);
    pxa_esp_surface_take_display_input_metrics(&input_metrics);
    assert(input_metrics.sample_to_guest_count == 0);
    pxa_esp_surface_take_input_metrics(&input_metrics);
    assert(input_metrics.sample_to_guest_count == 1 &&
           input_metrics.sample_to_guest_total_us == 600);
    pxa_esp_surface_perf_clear();
    {
        pxa_esp_surface_perf_info_t info;
        uint32_t values[4], count;
        test_now_us = 1000;
        assert(pxa_esp_surface_perf_start_display());
        pxa_esp_surface_note_game_frame_presented(1, 1100);
        pxa_esp_surface_note_display_frame_presented(0);
        pxa_esp_surface_note_display_frame_presented(900);
        pxa_esp_surface_note_display_frame_presented(1200);
        pxa_esp_surface_note_display_frame_presented(1200);
        pxa_esp_surface_note_display_frame_presented(1199);
        pxa_esp_surface_note_display_frame_presented(2200);
        test_now_us = 3000;
        assert(pxa_esp_surface_perf_stop(&info));
        assert(info.display_mode && info.window_us == 2000 &&
               info.completed_frames == 2 && info.present_interval_count == 1 &&
               info.first_delay_us == 200 && info.max_interval_us == 1000 &&
               info.tail_delay_us == 800 &&
               info.raster_count == 0 && !info.surface_changed);
        pxa_esp_surface_note_display_frame_presented(3200);
        assert(pxa_esp_surface_perf_read(PXA_ESP_SURFACE_PERF_PRESENT_INTERVAL,
                                       0, values, 4, &count));
        assert(count == 1 && values[0] == 1000);
        assert(pxa_esp_surface_perf_start_display());
        for (unsigned n = 0; n < PXA_ESP_SURFACE_PERF_CAPACITY + 3; ++n)
            pxa_esp_surface_note_display_frame_presented(4000 + n * 1000);
        test_now_us = 4000 + (PXA_ESP_SURFACE_PERF_CAPACITY + 2) * 1000 + 2500000;
        pxa_esp_surface_note_display_frame_presented(test_now_us);
        test_now_us += 100;
        assert(pxa_esp_surface_perf_stop(&info));
        assert(info.present_interval_count == PXA_ESP_SURFACE_PERF_CAPACITY &&
               info.present_interval_overflow == 3 && info.max_interval_us == 2500000 &&
               info.first_delay_us == 1000 && info.tail_delay_us == 100 &&
               info.completed_frames == PXA_ESP_SURFACE_PERF_CAPACITY + 4);
        assert(pxa_esp_surface_perf_start_display());
        test_now_us += 750000;
        assert(pxa_esp_surface_perf_stop(&info));
        assert(info.completed_frames == 0 && info.max_interval_us == 0 &&
               info.first_delay_us == 0 && info.tail_delay_us == 750000);
        test_now_us += 100;
        assert(pxa_esp_surface_perf_stop(&info));
        assert(info.window_us == 750000 && info.tail_delay_us == info.window_us);
        pxa_esp_surface_perf_clear();
        pxa_esp_surface_memory_snapshot(&memory_info);
        assert(memory_info.probe_bytes == 0);
    }
    pxa_esp_surface_release_frame(frame.lease);
    assert(release_notifications == 3);
    assert(backend.peek_release(backend.context, surface, &released) ==
           PXA_STATUS_WOULD_BLOCK);
    assert(backend.query(backend.context, surface, &state) == PXA_STATUS_OK &&
           state.presented_frames == 2 && state.released_frames == 3 &&
           state.free_buffers == 2);
    /* No new Guest frame: volume/modal redraws still have a leased backdrop.
     * The held buffer is neither writable nor released to the Guest. */
    pxa_esp_surface_set_system_overlay_visible(true);
    assert(!pxa_esp_surface_acquire_latest_native_for_direct(&probe));
    assert(!pxa_esp_surface_acquire_latest_for_direct(&probe));
    for (unsigned redraw = 0; redraw < 4; ++redraw) {
        assert(pxa_esp_surface_acquire_latest(&probe) &&
               probe.frame_id == 4 && probe.pixels == mapped_pixels + 64);
        pxa_esp_surface_release_frame(probe.lease);
    }
    pxa_esp_surface_set_system_overlay_visible(false);
    assert(backend.query(backend.context, surface, &state) == PXA_STATUS_OK &&
           state.presented_frames == 2 && state.released_frames == 3);
    assert(backend.acquire_buffer(backend.context, surface, &buffer_index) == PXA_STATUS_OK && buffer_index != 2);
    assert(backend.present_buffer(backend.context, surface, buffer_index, 5) == PXA_STATUS_OK);
    assert(pxa_esp_surface_acquire_latest_for_direct(&probe) && probe.frame_id == 5);
    assert(backend.peek_release(backend.context, surface, &released) == PXA_STATUS_OK &&
           released.buffer_index == 2 && released.frame_id == 4);
    backend.consume_release(backend.context, surface);
    /* Old storage is reusable while the replacement is still being rotated. */
    assert(backend.acquire_buffer(backend.context, surface, &buffer_index) == PXA_STATUS_OK);
    assert(backend.present_buffer(backend.context, surface, buffer_index, 6) == PXA_STATUS_OK);
    pxa_esp_surface_release_frame(probe.lease);
    backend.close(backend.context, surface);
    assert(allocations == 0);

    /* The production NES uses exactly two mapped buffers. One retained
     * backdrop must not deadlock replacement or expose the writer's pixels. */
    desc.buffer_count = 2;
    assert(backend.create(backend.context, &desc, &surface, &stride) == PXA_STATUS_OK);
    assert(backend.configure(backend.context, surface, &layer) == PXA_STATUS_OK);
    assert(backend.register_buffers(backend.context, surface, mapped_pixels, 64) == PXA_STATUS_OK);
    for (uint64_t id = 1; id <= 32; ++id) {
        assert(backend.acquire_buffer(backend.context, surface, &buffer_index) == PXA_STATUS_OK);
        memset(mapped_pixels + buffer_index * 32, (int)id, 32);
        assert(backend.present_buffer(backend.context, surface, buffer_index, id) == PXA_STATUS_OK);
        assert(pxa_esp_surface_acquire_latest(&probe) && probe.frame_id == id);
        if (id > 1) {
            assert(backend.peek_release(backend.context, surface, &released) == PXA_STATUS_OK && released.frame_id == id - 1);
            backend.consume_release(backend.context, surface);
            /* Return the previous storage before the replacement lease ends. */
            assert(backend.query(backend.context, surface, &state) == PXA_STATUS_OK && state.free_buffers == 1);
        }
        pxa_esp_surface_release_frame(probe.lease);
        assert(backend.peek_release(backend.context, surface, &released) == PXA_STATUS_WOULD_BLOCK);
        for (unsigned redraw = 0; redraw < 3; ++redraw) {
            assert(pxa_esp_surface_acquire_current_for_preview(&probe) && probe.frame_id == id);
            assert(probe.pixels[0] == (uint8_t)id);
            pxa_esp_surface_release_frame(probe.lease);
        }
    }
    /* A volume-style alpha plane must blend over the retained Guest frame,
     * rather than promoting the empty LVGL background to a fullscreen cover.
     * No Guest submit occurs while the overlay animates or disappears. */
    pxa_esp_surface_set_system_alpha_provider(ui_alpha_plane, &notifications);
    pxa_esp_surface_runtime_modal_enter();
    assert(pxa_esp_surface_composition_required());
    for (unsigned redraw = 0; redraw < 8; ++redraw) {
        assert(!pxa_esp_surface_acquire_latest_native_for_direct(&probe));
    assert(!pxa_esp_surface_acquire_latest_for_direct(&probe));
        assert(pxa_esp_surface_acquire_latest(&probe) && probe.frame_id == 32);
        assert(probe.opaque_ui_region_count == 0 && !probe.suppress_guest_alpha);
        assert(probe.system_alpha_plane.visible && probe.system_alpha_plane.alpha);
        const uint16_t backdrop = pxa_read_u16(probe.pixels);
        assert(backdrop == 0x2020);
        assert(pxa_esp_surface_blend_alpha_pixel(backdrop,
                   probe.system_alpha_plane.pixels[0],
                   probe.system_alpha_plane.alpha[0],255) == backdrop);
        assert(pxa_esp_surface_blend_alpha_pixel(backdrop,
                   probe.system_alpha_plane.pixels[2],
                   probe.system_alpha_plane.alpha[2],255) == 0x001f);
        pxa_esp_surface_release_frame(probe.lease);
    }
    pxa_esp_surface_runtime_modal_leave();
    pxa_esp_surface_set_system_alpha_provider(empty_ui_alpha_plane, &notifications);
    assert(pxa_esp_surface_acquire_latest(&probe) && probe.frame_id == 32 &&
           !probe.system_alpha_plane.visible && !probe.opaque_ui_region_count);
    pxa_esp_surface_release_frame(probe.lease);
    assert(backend.query(backend.context,surface,&state) == PXA_STATUS_OK &&
           state.presented_frames == 32 && state.released_frames == 31 &&
           state.free_buffers == 1);
    pxa_esp_surface_set_system_alpha_provider(NULL,NULL);
    backend.close(backend.context, surface);
    assert(allocations == 0);

    {
        uint8_t upload[PXA_RASTER_UPLOAD_HEADER_BYTES +
                       PXA_RASTER_PALETTE_COLORS * 2u] = {0};
        uint8_t draw[PXA_RASTER_DRAW_HEADER_BYTES + PXA_RASTER_CLEAR_BYTES] =
            {0};
        pxa_raster_telemetry_t telemetry;
        pxa_esp_surface_perf_info_t perf_info;
        uint32_t perf_values[4] = {0};
        uint32_t perf_count = 0;
        uint16_t *front;
        unsigned color;
        pxa_game_render_desc_t game_desc = {
            4, 4, 3, PXA_GAME_RENDER_FLAG_PREFER_DIRECT_SCANOUT,
            PXA_GAME_RENDER_SCRATCH_DEPTH16, 0};
        uint32_t capabilities;
        assert(game_backend.create(game_backend.context, &game_desc, &surface,
                                   &capabilities) == PXA_STATUS_OK);
        assert((capabilities & (PXA_RASTER_CAP_SPRITE_BATCH |
                                PXA_RASTER_CAP_TRIANGLE_BATCH |
                                PXA_RASTER_CAP_COVERAGE_MASK)) ==
               (PXA_RASTER_CAP_SPRITE_BATCH |
                PXA_RASTER_CAP_TRIANGLE_BATCH |
                PXA_RASTER_CAP_COVERAGE_MASK));
        assert(allocations == 8);
        pxa_esp_surface_memory_snapshot(&memory_info);
        assert(memory_info.frame_bytes >= 3u * sizeof(first) &&
               memory_info.scratch_bytes >= 4u * 4u * sizeof(uint16_t) &&
               memory_info.mailbox_bytes >=
                   3u * PXA_ESP_RASTER_MAILBOX_MIN_BYTES &&
               memory_info.probe_bytes == 0);
        assert(((pxa_esp_surface_t *)(uintptr_t)surface)
                       ->raster_draw_capacities[0] ==
                   PXA_ESP_RASTER_MAILBOX_MIN_BYTES &&
               ((pxa_esp_surface_t *)(uintptr_t)surface)
                       ->raster_draw_capacities[1] ==
                   PXA_ESP_RASTER_MAILBOX_MIN_BYTES);
        pxa_write_u32(upload, PXA_RASTER_UPLOAD_MAGIC);
        pxa_write_u16(upload + 4, PXA_RASTER_ABI_MAJOR);
        pxa_write_u16(upload + 6, PXA_RASTER_ABI_MINOR);
        upload[8] = PXA_RASTER_UPLOAD_PALETTE_RGB565;
        pxa_write_u16(upload + 12, PXA_RASTER_PALETTE_COLORS);
        pxa_write_u16(upload + 14, 1);
        pxa_write_u32(upload + 16, PXA_RASTER_PALETTE_COLORS * 2u);
        pxa_write_u16(upload + PXA_RASTER_UPLOAD_HEADER_BYTES, 0x5aa5);
        assert(game_backend.upload(game_backend.context, surface, upload,
                                   sizeof(upload)) == PXA_STATUS_OK);
        {
            pxa_raster_resources_t view;
            pxa_raster_bindings_view(
                &((pxa_esp_surface_t *)(uintptr_t)surface)->raster_bindings,
                capabilities, &view);
            assert(allocations == 9 && view.palette[0] == 0x5aa5);
        }

        pxa_write_u32(draw, PXA_RASTER_DRAW_MAGIC);
        pxa_write_u16(draw + 4, PXA_RASTER_ABI_MAJOR);
        pxa_write_u16(draw + 6, PXA_RASTER_ABI_MINOR);
        pxa_write_u32(draw + 8, sizeof(draw));
        pxa_write_u32(draw + 16, 1);
        pxa_write_u64(draw + 20, 1);
        draw[PXA_RASTER_DRAW_HEADER_BYTES] = PXA_RASTER_RECORD_CLEAR_RGB565;
        pxa_write_u16(draw + PXA_RASTER_DRAW_HEADER_BYTES + 2,
                      PXA_RASTER_CLEAR_BYTES);
        pxa_write_u16(draw + PXA_RASTER_DRAW_HEADER_BYTES + 4, 0x1234);
        assert(pxa_esp_surface_perf_start());
        pxa_esp_surface_memory_snapshot(&memory_info);
        assert(memory_info.probe_bytes >=
               sizeof(pxa_esp_surface_perf_buffer_t));
        assert(!pxa_esp_surface_perf_read(PXA_ESP_SURFACE_PERF_RASTER, 0,
                                          perf_values, 4, &perf_count));
        assert(game_backend.submit(game_backend.context, surface, draw,
                                   sizeof(draw)) == PXA_STATUS_OK);
        assert(pxa_esp_surface_has_pending_frame());
        assert(((pxa_esp_surface_t *)(uintptr_t)surface)->pending ==
               PXA_ESP_SURFACE_NONE);
        assert(pxa_esp_surface_acquire_latest_for_direct(&frame));
        assert(frame.frame_id == 1);
        assert(frame.raster_ready_tick_us == (uint32_t)test_now_us);
        front = (uint16_t *)(uintptr_t)frame.pixels;
        for (color = 0; color < 16; ++color) assert(front[color] == 0x1234);
        pxa_esp_surface_release_frame(frame.lease);
        // Pai-touch releases after rotation, before the actual LCD callback.
        // Keep that completion latency even with no live Surface acquisition.
        const uint64_t complete_us = (UINT64_C(1) << 32) + 1000;
        const uint32_t ready_tick = (uint32_t)(complete_us - 19000);
        pxa_esp_surface_note_display_latency(1, 0, ready_tick, complete_us);
        pxa_esp_surface_note_display_latency(1, 0, ready_tick, complete_us + 1000);
        assert(game_backend.query(game_backend.context, surface, &telemetry) ==
               PXA_STATUS_OK && telemetry.present_us == 19000);
        assert(pxa_esp_surface_acquire_current_for_preview(&frame));
        assert(frame.raster_ready_tick_us == (uint32_t)test_now_us);
        pxa_esp_surface_release_frame(frame.lease);
        pxa_esp_surface_note_game_frame_presented(1, test_now_us + 1000);

        pxa_write_u64(draw + 20, 2);
        pxa_write_u16(draw + PXA_RASTER_DRAW_HEADER_BYTES + 4, 0x2222);
        assert(game_backend.submit(game_backend.context, surface, draw,
                                   sizeof(draw)) == PXA_STATUS_OK);
        pxa_write_u64(draw + 20, 3);
        pxa_write_u16(draw + PXA_RASTER_DRAW_HEADER_BYTES + 4, 0x3333);
        assert(game_backend.submit(game_backend.context, surface, draw,
                                   sizeof(draw)) == PXA_STATUS_OK);
        /* A rejected list must not remove the valid pending frame. */
        pxa_write_u64(draw + 20, 4);
        draw[PXA_RASTER_DRAW_HEADER_BYTES] = 0xff;
        assert(game_backend.submit(game_backend.context, surface, draw,
                                   sizeof(draw)) == PXA_STATUS_UNSUPPORTED);
        assert(((pxa_esp_surface_t *)(uintptr_t)surface)->raster_draw_pending !=
               PXA_ESP_SURFACE_NONE);
        draw[PXA_RASTER_DRAW_HEADER_BYTES] = PXA_RASTER_RECORD_CLEAR_RGB565;
        assert(pxa_esp_surface_acquire_latest_for_direct(&frame));
        assert(frame.frame_id == 3);
        front = (uint16_t *)(uintptr_t)frame.pixels;
        for (color = 0; color < 16; ++color) assert(front[color] == 0x3333);
        assert(game_backend.query(game_backend.context, surface, &telemetry) ==
                   PXA_STATUS_OK &&
               telemetry.submitted_frames == 3 &&
               telemetry.dropped_frames == 1 &&
               telemetry.rendered_frames == 2 &&
               telemetry.visible_frames == 1 &&
               telemetry.clear_commands == 2 &&
               telemetry.last_draw_list_bytes == sizeof(draw));
        pxa_esp_surface_release_frame(frame.lease);
        test_now_us += 2000;
        pxa_esp_surface_note_game_frame_presented(3, test_now_us);
        pxa_esp_surface_note_game_frame_presented(3, test_now_us + 500);
        assert(pxa_esp_surface_perf_stop(&perf_info));
        assert(perf_info.raster_count == 2 &&
               perf_info.present_interval_count == 1 &&
               perf_info.raster_overflow == 0 &&
               perf_info.present_interval_overflow == 0 &&
               perf_info.surface_changed == 0);
        assert(pxa_esp_surface_perf_read(PXA_ESP_SURFACE_PERF_RASTER, 0,
                                         perf_values, 4, &perf_count) &&
               perf_count == 2);
        assert(pxa_esp_surface_perf_read(
                   PXA_ESP_SURFACE_PERF_PRESENT_INTERVAL, 0,
                   perf_values, 4, &perf_count) &&
               perf_count == 1 && perf_values[0] == 1000);
        pxa_esp_surface_perf_clear();
        pxa_esp_surface_memory_snapshot(&memory_info);
        assert(memory_info.probe_bytes == 0);
        {
            enum {
                BIG_COMMANDS = 512,
                BIG_DRAW_BYTES = PXA_RASTER_DRAW_HEADER_BYTES +
                                 BIG_COMMANDS * PXA_RASTER_CLEAR_BYTES
            };
            uint8_t big_draw[BIG_DRAW_BYTES] = {0};
            unsigned command;
            pxa_write_u32(big_draw, PXA_RASTER_DRAW_MAGIC);
            pxa_write_u16(big_draw + 4, PXA_RASTER_ABI_MAJOR);
            pxa_write_u16(big_draw + 6, PXA_RASTER_ABI_MINOR);
            pxa_write_u32(big_draw + 8, sizeof(big_draw));
            pxa_write_u32(big_draw + 16, BIG_COMMANDS);
            pxa_write_u64(big_draw + 20, 4);
            for (command = 0; command < BIG_COMMANDS; ++command) {
                uint8_t *record = big_draw + PXA_RASTER_DRAW_HEADER_BYTES +
                                  command * PXA_RASTER_CLEAR_BYTES;
                record[0] = PXA_RASTER_RECORD_CLEAR_RGB565;
                pxa_write_u16(record + 2, PXA_RASTER_CLEAR_BYTES);
                pxa_write_u16(record + 4, 0x4567);
            }
            assert(game_backend.submit(game_backend.context, surface, big_draw,
                                       sizeof(big_draw)) == PXA_STATUS_OK);
            pxa_esp_surface_memory_snapshot(&memory_info);
            assert(memory_info.mailbox_bytes >
                       3u * PXA_ESP_RASTER_MAILBOX_MIN_BYTES &&
                   memory_info.peak_total_bytes >
                       memory_info.frame_bytes + memory_info.scratch_bytes +
                           memory_info.mailbox_bytes);
            assert(((pxa_esp_surface_t *)(uintptr_t)surface)
                           ->raster_draw_capacities[0] ==
                       PXA_ESP_RASTER_MAILBOX_MIN_BYTES * 2u &&
                   allocations == 9);
            pxa_write_u64(big_draw + 20, 5);
            fail_next_malloc = 1;
            assert(game_backend.submit(game_backend.context, surface,
                                       big_draw, sizeof(big_draw)) ==
                   PXA_STATUS_RESOURCE_LIMIT);
            assert(fail_next_malloc == 0 &&
                   ((pxa_esp_surface_t *)(uintptr_t)surface)
                           ->raster_draw_pending != PXA_ESP_SURFACE_NONE);
            assert(pxa_esp_surface_acquire_latest_for_direct(&frame) &&
                   frame.frame_id == 4);
            front = (uint16_t *)(uintptr_t)frame.pixels;
            for (color = 0; color < 16; ++color)
                assert(front[color] == 0x4567);
            pxa_esp_surface_release_frame(frame.lease);
        }
        game_backend.close(game_backend.context, surface);
        assert(allocations == 0);
        pxa_esp_surface_memory_snapshot(&memory_info);
        assert(memory_info.frame_bytes == 0 && memory_info.scratch_bytes == 0 &&
               memory_info.mailbox_bytes == 0 && memory_info.probe_bytes == 0 &&
               memory_info.peak_total_bytes > 0);

        game_desc.scratch_mode = PXA_GAME_RENDER_SCRATCH_NONE;
        game_desc.max_draw_bytes = 8192;
        assert(game_backend.create(game_backend.context, &game_desc, &surface,
                                   &capabilities) == PXA_STATUS_OK);
        assert(allocations == 7 &&
               ((pxa_esp_surface_t *)(uintptr_t)surface)->raster_depth_buffer ==
                   NULL);
        assert(((pxa_esp_surface_t *)(uintptr_t)surface)
                       ->raster_draw_capacities[0] == 8192 &&
               ((pxa_esp_surface_t *)(uintptr_t)surface)
                       ->raster_draw_capacities[2] == 8192);
        assert(game_backend.upload(game_backend.context, surface, upload,
                                   sizeof(upload)) == PXA_STATUS_OK);
        pxa_write_u64(draw + 20, 1);
        pxa_write_u32(draw + 12, PXA_RASTER_CAP_COVERAGE_MASK);
        assert(game_backend.submit(game_backend.context, surface, draw,
                                   sizeof(draw)) == PXA_STATUS_BAD_STATE);
        pxa_write_u32(draw + 12, 0);
        assert(game_backend.submit(game_backend.context, surface, draw,
                                   sizeof(draw)) == PXA_STATUS_OK);
        assert(pxa_esp_surface_acquire_latest_for_direct(&frame));
        pxa_esp_surface_release_frame(frame.lease);
        game_backend.close(game_backend.context, surface);
        assert(allocations == 0);

        game_desc.scratch_mode = PXA_GAME_RENDER_SCRATCH_COVERAGE_2BIT;
        assert(game_backend.create(game_backend.context, &game_desc, &surface,
                                   &capabilities) == PXA_STATUS_OK);
        assert(allocations == 8 && last_aligned_allocation_size == 8 &&
               ((pxa_esp_surface_t *)(uintptr_t)surface)->raster_depth_buffer !=
                   NULL);
        assert(game_backend.upload(game_backend.context, surface, upload,
                                   sizeof(upload)) == PXA_STATUS_OK);
        pxa_write_u32(draw + 12, PXA_RASTER_CAP_COVERAGE_MASK);
        assert(game_backend.submit(game_backend.context, surface, draw,
                                   sizeof(draw)) == PXA_STATUS_OK);
        assert(pxa_esp_surface_acquire_latest_for_direct(&frame));
        pxa_esp_surface_release_frame(frame.lease);
        game_backend.close(game_backend.context, surface);
        assert(allocations == 0);

        /* A clear-only DrawList needs no palette allocation or upload. */
        game_desc.scratch_mode = PXA_GAME_RENDER_SCRATCH_NONE;
        assert(game_backend.create(game_backend.context, &game_desc, &surface,
                                   &capabilities) == PXA_STATUS_OK);
        assert(((pxa_esp_surface_t *)(uintptr_t)surface)->raster_bindings.palette ==
               NULL);
        pxa_write_u64(draw + 20, 1);
        pxa_write_u32(draw + 12, 0);
        pxa_write_u16(draw + PXA_RASTER_DRAW_HEADER_BYTES + 4, 0xf800);
        assert(game_backend.submit(game_backend.context, surface, draw,
                                   sizeof(draw)) == PXA_STATUS_OK);
        assert(pxa_esp_surface_acquire_latest_for_direct(&frame));
        front = (uint16_t *)(uintptr_t)frame.pixels;
        for (color = 0; color < 16; ++color) assert(front[color] == 0xf800);
        pxa_esp_surface_release_frame(frame.lease);
        game_backend.close(game_backend.context, surface);
        assert(allocations == 0);
    }

    raster_snapshot_scenario(&game_backend, snapshot_present, NULL);
    test_close_while_rendering(&game_backend);
    test_prepared_pipeline(&game_backend);
    test_borrowed_submit(&game_backend);
    test_replace_while_rendering(&game_backend);
    test_retired_display_budget(&game_backend);
    assert(allocations == 0);
    assert(pxa_esp_resource_memory_end() == PXA_STATUS_OK);
    /* Delayed consumers keep their old descriptors while the next activation
     * starts. Closing blocks late allocation, not late free or app startup. */
    assert(pxa_esp_resource_memory_begin() == PXA_STATUS_OK);
    const pxa_memory_allocator_t *old_allocator = pxa_esp_resource_allocator(PXA_MEMORY_EXTERNAL, PXA_MEMORY_RASTER);
    void *retired = pxa_memory_allocate(old_allocator, 128);
    assert(retired && pxa_esp_resource_memory_end() == PXA_STATUS_WOULD_BLOCK);
    assert(pxa_esp_resource_memory_begin() == PXA_STATUS_OK);
    const pxa_memory_allocator_t *new_allocator = pxa_esp_resource_allocator(PXA_MEMORY_EXTERNAL, PXA_MEMORY_RASTER);
    assert(new_allocator != old_allocator && new_allocator->owner != old_allocator->owner);
    assert(pxa_memory_allocate(old_allocator, 1) == NULL);
    void *current = pxa_memory_allocate(new_allocator, 128);
    assert(current && pxa_esp_resource_memory_end() == PXA_STATUS_WOULD_BLOCK);
    assert(pxa_esp_resource_memory_begin() == PXA_STATUS_RESOURCE_LIMIT);
    pxa_memory_release(retired);
    assert(pxa_esp_resource_memory_begin() == PXA_STATUS_OK);
    pxa_memory_release(current);
    assert(pxa_esp_resource_memory_end() == PXA_STATUS_OK && !allocations);
    /* Device storage shares global limits but survives app generations and
     * repeated initialization without resetting counters or descriptors. */
    const pxa_memory_allocator_t *device_allocator = pxa_esp_device_resource_allocator(
        PXA_MEMORY_EXTERNAL, PXA_MEMORY_METADATA);
    assert(device_allocator);
    void *device = pxa_memory_allocate(device_allocator, 4096);
    assert(device && pxa_esp_resource_memory_initialize() == PXA_STATUS_OK);
    assert(pxa_esp_resource_memory_begin() == PXA_STATUS_OK);
    const pxa_memory_allocator_t *app_allocator = pxa_esp_resource_allocator(
        PXA_MEMORY_EXTERNAL, PXA_MEMORY_RASTER);
    assert(device_allocator->owner != app_allocator->owner);
    assert(!pxa_memory_allocate(app_allocator, CONFIG_PXA_RESOURCE_EXTERNAL_BYTES -
        (pxa_memory_allocation_bytes(1) - 1)));
    pxa_memory_stats_t device_stats;
    pxa_esp_resource_memory_stats(&device_stats, NULL);
    assert(device_stats.charged[PXA_MEMORY_EXTERNAL] == pxa_memory_allocation_bytes(4096));
    assert(pxa_esp_resource_memory_end() == PXA_STATUS_OK);
    pxa_memory_release(device);
    assert(!allocations);
    /* Actual ESP allocator configuration, including prefix bytes, is shared
     * by device and active/retiring app owners. Unrelated raster still fits. */
    assert(pxa_esp_resource_memory_begin() == PXA_STATUS_OK);
    const pxa_memory_allocator_t *temporary = pxa_esp_resource_allocator(
        PXA_MEMORY_EXTERNAL, PXA_MEMORY_TEMPORARY);
    size_t prefix = pxa_memory_allocation_bytes(1) - 1;
    void *decode = pxa_memory_allocate(temporary, CONFIG_PXA_RESOURCE_TEMPORARY_EXTERNAL_BYTES - prefix);
    assert(decode);
    assert(!pxa_memory_allocate(temporary, 1));
    const pxa_memory_allocator_t *device_temporary = pxa_esp_device_resource_allocator(
        PXA_MEMORY_EXTERNAL, PXA_MEMORY_TEMPORARY);
    assert(!pxa_memory_allocate(device_temporary, 1));
    void *texture = pxa_esp_resource_allocate(PXA_MEMORY_EXTERNAL, PXA_MEMORY_RASTER, 128);
    assert(texture);
    pxa_esp_resource_memory_stats(&device_stats, NULL);
    assert(device_stats.temporary_peak[PXA_MEMORY_EXTERNAL] == CONFIG_PXA_RESOURCE_TEMPORARY_EXTERNAL_BYTES);
    assert(pxa_esp_resource_memory_end() == PXA_STATUS_WOULD_BLOCK);
    assert(!pxa_memory_allocate(device_temporary, 1));
    pxa_memory_release(decode); pxa_memory_release(texture);
    decode = pxa_memory_allocate(device_temporary, 1); assert(decode);
    pxa_memory_release(decode);
    assert(pxa_esp_resource_memory_end() == PXA_STATUS_OK && !allocations);
    pxa_esp_surface_set_frame_ready_callback(NULL, NULL);
    pxa_esp_surface_set_release_ready_callback(NULL, NULL);
    pxa_surface_desc_t overlay_desc = {
        .width = 4, .height = 4, .format = PXA_SURFACE_FORMAT_RGB565,
        .buffer_count = 2};
    pxa_surface_layer_t overlay_layer = {
        .width = 4, .height = 4, .visible = 1};
    assert(pxa_esp_resource_memory_begin() == PXA_STATUS_OK);
    pxa_esp_surface_set_system_alpha_provider(ui_alpha_plane, &notifications);
    assert(backend.create(backend.context, &overlay_desc, &surface, &stride) ==
           PXA_STATUS_OK);
    assert(backend.configure(backend.context, surface, &overlay_layer) ==
           PXA_STATUS_OK);
    assert(backend.write(backend.context, surface, first, sizeof(first)) ==
           PXA_STATUS_OK);
    assert(backend.queue(backend.context, surface, 12, NULL, 0) ==
           PXA_STATUS_OK);
    assert(pxa_esp_surface_composition_required());
    assert(!pxa_esp_surface_acquire_latest_native_for_direct(&probe));
    assert(!pxa_esp_surface_acquire_latest_for_direct(&probe));
    assert(pxa_esp_surface_acquire_prepared_with_alpha(&probe));
    assert(probe.system_alpha_plane.visible && !probe.suppress_guest_alpha &&
           probe.opaque_ui_region_count == 0);
    pxa_esp_surface_release_frame(probe.lease);
    pxa_esp_surface_set_power_overlay_visible(true);
    assert(!pxa_esp_surface_acquire_prepared_with_alpha(&probe));
    pxa_esp_surface_set_power_overlay_visible(false);
    // Even valid alpha planes cannot bypass the fresh-frame resume barrier.
    assert(!pxa_esp_surface_acquire_prepared_with_alpha(&probe));
    assert(backend.write(backend.context, surface, first, sizeof(first)) == PXA_STATUS_OK);
    assert(backend.queue(backend.context, surface, 13, NULL, 0) == PXA_STATUS_OK);
    assert(pxa_esp_surface_acquire_latest(&probe));
    assert(probe.system_alpha_plane.visible &&
           probe.system_alpha_plane.pixels != NULL &&
           probe.ui_alpha_plane.pixels == NULL &&
           !probe.suppress_guest_alpha);
    pxa_esp_surface_release_frame(probe.lease);
    pxa_esp_surface_set_system_alpha_provider(empty_ui_alpha_plane,
                                               &notifications);
    pxa_esp_surface_runtime_modal_enter();
    assert(pxa_esp_surface_acquire_prepared_with_alpha(&probe));
    pxa_esp_surface_release_frame(probe.lease);
    assert(pxa_esp_surface_acquire_latest(&probe));
    assert(probe.opaque_ui_region_count == 0 &&
           !probe.suppress_guest_alpha);
    pxa_esp_surface_release_frame(probe.lease);
    pxa_esp_surface_set_system_alpha_provider(missing_ui_alpha_plane,
                                               &notifications);
    pxa_esp_surface_set_ui_alpha_provider(failed_ui_alpha_plane, &notifications);
    assert(!pxa_esp_surface_acquire_prepared_with_alpha(&probe));
    assert(g_acquired_surface == NULL);
    pxa_esp_surface_set_ui_alpha_provider(NULL, NULL);
    pxa_esp_surface_set_system_alpha_provider(failed_ui_alpha_plane, &notifications);
    assert(!pxa_esp_surface_acquire_prepared_with_alpha(&probe));
    assert(g_acquired_surface == NULL);
    pxa_esp_surface_set_system_alpha_provider(missing_ui_alpha_plane, &notifications);
    assert(pxa_esp_surface_composition_required());
    assert(!pxa_esp_surface_acquire_prepared_with_alpha(&probe));
    assert(pxa_esp_surface_acquire_latest(&probe));
    assert(probe.opaque_ui_region_count == 1 &&
           probe.opaque_ui_regions[0].width == 4 &&
           probe.suppress_guest_alpha);
    pxa_esp_surface_release_frame(probe.lease);
    pxa_esp_surface_runtime_modal_leave();
    pxa_esp_surface_set_system_alpha_provider(empty_ui_alpha_plane,
                                               &notifications);
    assert(!pxa_esp_surface_acquire_prepared_with_alpha(&probe));
    assert(pxa_esp_surface_acquire_latest(&probe));
    assert(probe.opaque_ui_region_count == 0 &&
           !probe.suppress_guest_alpha);
    pxa_esp_surface_release_frame(probe.lease);
    backend.close(backend.context, surface);
    pxa_esp_surface_set_system_alpha_provider(NULL, NULL);
    check_producer_front_buffer(&backend);
    check_native_producer(&backend);
    check_inline_slots(&backend);
    check_single_readback(&backend);
    check_ordered_producer(&backend);
    assert(pxa_esp_resource_memory_end() == PXA_STATUS_OK && !allocations);
    return 0;
}
