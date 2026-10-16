/* Optional A/B against an explicit saved image_decode.c. Same target and
 * allocation policy, only its blit function changes. Never linked by products. */
#include "pxa/image_decode.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern pxa_status_t before_pxa_image_target_blit(pxa_image_target_t *, const pxa_image_tile_t *);
static int before, format, rate, points;
static uint8_t *input;
static pxa_image_target_t *target;
static pxa_status_t open_codec(pxa_bytes_t signature, pxa_image_target_t *t,
                               const pxa_memory_allocator_t *memory, void **s) {
    (void)signature; (void)memory;
    *s = target = t;
    return PXA_STATUS_OK;
}
static pxa_status_t write_codec(void *s, pxa_bytes_t bytes) { (void)s; (void)bytes; return 0; }
static void close_codec(void *s) { (void)s; }
static pxa_status_t finish_codec(void *s) {
    (void)s;
    pxa_status_t status = pxa_image_target_begin(target, 640, 384, format == PXA_IMAGE_RGBA8888);
    unsigned bpp = format == PXA_IMAGE_RGB565 ? 2 : format == PXA_IMAGE_RGB888 ? 3 : 4;
    unsigned stride = 640 / rate * bpp, size = points ? 1 : 16;
    for (unsigned y = 0; !status && y < 384; y += size * rate)
        for (unsigned x = 0; !status && x < 640; x += size * rate) {
            pxa_image_tile_t tile = {x, y, size * rate, size * rate, size, size, stride,
                (uint8_t)format, input + y / rate * stride + x / rate * bpp};
            status = before ? before_pxa_image_target_blit(target, &tile)
                            : pxa_image_target_blit(target, &tile);
        }
    return status;
}
static void *allocate(void *context, size_t bytes) {
    (void)context; return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
static void release(void *context, void *memory) { (void)context; heap_caps_free(memory); }
void blit_benchmark(void) {
    static const pxa_image_codec_t codec = {.open = open_codec, .write = write_codec, .finish = finish_codec, .close = close_codec};
    static const pxa_image_codec_t *const codecs[] = {&codec};
    static const pxa_image_backend_t backend = {codecs, 1};
    input = allocate(NULL, 640 * 384 * 4);
    if (!input) abort();
    for (unsigned i = 0; i < 640 * 384 * 4; ++i) input[i] = (uint8_t)(i * 13 + i / 71);
    const int cases[][3] = {{PXA_IMAGE_RGB888, 1, 0}, {PXA_IMAGE_RGB888, 2, 0},
                           {PXA_IMAGE_RGB888, 4, 0}, {PXA_IMAGE_RGB565, 1, 0},
                           {PXA_IMAGE_RGBA8888, 1, 1}, {PXA_IMAGE_RGBA8888, 8, 1}};
    for (unsigned test = 0; test < sizeof(cases) / sizeof(cases[0]); ++test) {
        format = cases[test][0]; rate = cases[test][1]; points = cases[test][2];
        unsigned reference = 0;
        for (before = 1; before >= 0; --before) {
            pxa_memory_budget_t budget;
            pxa_memory_budget_config_t config = {0};
            config.limit[1] = config.temporary_limit[1] = 1024 * 1024;
            if (pxa_memory_budget_init(&budget, &config)) abort();
            pxa_memory_owner_t owner;
            if (pxa_memory_owner_open(&budget, config.limit, &owner)) abort();
            pxa_memory_allocator_t memory = {&budget, owner, 1, PXA_MEMORY_IMAGE,
                                             NULL, allocate, release};
            pxa_memory_allocator_t temporary = memory; temporary.kind = PXA_MEMORY_TEMPORARY;
            pxa_image_decode_options_t options = {.max_width=241, .max_height=512, .max_pixels=32768, .max_encoded_bytes=1024};
            int64_t elapsed = 0;
            unsigned hash = 0;
            for (unsigned repeat = 0; repeat < 9; ++repeat) {
                pxa_image_decoder_t *decoder;
                pxa_decoded_image_t decoded;
                const uint8_t signature[8] = {1};
                if (pxa_image_decoder_create(&options, &memory, &temporary, &backend, &decoder) ||
                    pxa_image_decoder_write(decoder, (pxa_bytes_t){signature, 8})) abort();
                int64_t start = esp_timer_get_time();
                if (pxa_image_decoder_finish(decoder, &decoded)) abort();
                if (repeat) elapsed += esp_timer_get_time() - start;
                pxa_image_decoder_destroy(decoder);
                pxa_asset_object_view_t view;
                pxa_asset_object_view(decoded.image, &view);
                hash = 2166136261u;
                for (unsigned i = 0; i < view.bytes; ++i) hash = (hash ^ view.data[i]) * 16777619u;
                pxa_asset_object_release(decoded.image);
            }
            if (before) reference = hash;
            else if (hash != reference) abort();
            pxa_memory_stats_t stats;
            if (pxa_memory_budget_stats(&budget, owner, &stats) || stats.charged[1]) abort();
            printf("BLIT_BENCH {\"case\":%u,\"before\":%d,\"mean_us\":%lld,"
                   "\"peak_external\":%u,\"checksum\":%u}\n", test, before,
                   (long long)(elapsed / 8), (unsigned)stats.peak[1], hash);
            fflush(stdout);
            if (pxa_memory_owner_close(&budget, owner)) abort();
        }
    }
    heap_caps_free(input);
}
