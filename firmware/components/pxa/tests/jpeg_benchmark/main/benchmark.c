/* Isolated decoder comparison, NOT application or LCD FPS. Flash only the app
 * partition and restore the saved production binary after collecting results.
 * Both codecs read PSRAM input and produce RGB565 LE in PSRAM. The reference
 * buffer is outside each measured window; only workspace/block scratch counts
 * toward the local heap peak. Full/scaled outputs have identical dimensions.
 */
#include "fixtures.h"
#include "esp_jpeg_dec.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rom/tjpgd.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { OUTPUT_CAPACITY = 640 * 384 * 2, REPEATS = 8 };
typedef struct {
    const char *name;
    const unsigned char *input;
    size_t bytes;
    unsigned width, height, scale;
} fixture_t;
typedef struct {
    const unsigned char *input;
    size_t bytes, position;
    unsigned char *output;
    unsigned width, height;
} rom_t;
static UINT read_rom(JDEC *decoder, BYTE *pixels, UINT count) {
    rom_t *s = decoder->device;
    if (count > s->bytes - s->position) count = s->bytes - s->position;
    if (pixels) memcpy(pixels, s->input + s->position, count);
    s->position += count;
    return count;
}
static UINT write_rom(JDEC *decoder, void *pixels, JRECT *rect) {
    rom_t *s = decoder->device;
    const unsigned char *p = pixels;
    for (unsigned y = rect->top; y <= rect->bottom; ++y)
        for (unsigned x = rect->left; x <= rect->right; ++x, p += 3) {
            if (x >= s->width || y >= s->height) return 0;
            unsigned value = ((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3);
            size_t at = ((size_t)y * s->width + x) * 2;
            s->output[at] = value; s->output[at + 1] = value >> 8;
        }
    return 1;
}
static int decode_rom(const fixture_t *f, const unsigned char *input, unsigned char *output) {
    void *workspace = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!workspace) return -99;
    rom_t state = {input, f->bytes, 0, output, f->width >> f->scale, f->height >> f->scale};
    JDEC decoder = {0};
    int result = jd_prepare(&decoder, read_rom, workspace, 8192, &state);
    if (!result) result = jd_decomp(&decoder, write_rom, f->scale);
    heap_caps_free(workspace);
    return result;
}
static int decode_new(const fixture_t *f, const unsigned char *input, unsigned char *output,
                      int block) {
    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    config.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    config.block_enable = block;
    if (f->scale) {
        config.scale.width = f->width >> f->scale;
        config.scale.height = f->height >> f->scale;
    }
    jpeg_dec_handle_t decoder = NULL;
    jpeg_dec_io_t io = {.inbuf = (unsigned char *)input, .inbuf_len = f->bytes, .outbuf = output};
    jpeg_dec_header_info_t info;
    int result = jpeg_dec_open(&config, &decoder), size = 0, count = 0;
    void *scratch = NULL;
    if (!result) result = jpeg_dec_parse_header(decoder, &io, &info);
    if (!result) result = jpeg_dec_get_outbuf_len(decoder, &size);
    if (!result && (size <= 0 || size > OUTPUT_CAPACITY)) result = -99;
    if (!result) result = jpeg_dec_get_process_count(decoder, &count);
    if (!result && block) {
        scratch = heap_caps_aligned_alloc(16, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!scratch) result = -99;
        io.outbuf = scratch;
    }
    size_t at = 0;
    for (int i = 0; !result && i < count; ++i) {
        result = jpeg_dec_process(decoder, &io);
        if (!result && block) {
            if (io.out_size < 0 || (size_t)io.out_size > OUTPUT_CAPACITY - at) result = -99;
            else { memcpy(output + at, scratch, io.out_size); at += io.out_size; }
        }
    }
    heap_caps_free(scratch);
    if (decoder) jpeg_dec_close(decoder);
    return result;
}
static void run(const fixture_t *f, int kind, unsigned char *input, unsigned char *output,
                unsigned char *reference, int reference_ok) {
    size_t before_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t before_external = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    heap_caps_monitor_local_minimum_free_size_start();
    int result = 0;
    int64_t elapsed = 0;
    for (unsigned i = 0; i < REPEATS; ++i) {
        memset(output, 0, OUTPUT_CAPACITY);
        int64_t start = esp_timer_get_time();
        result = kind ? decode_new(f, input, output, kind == 2) : decode_rom(f, input, output);
        elapsed += esp_timer_get_time() - start;
        if (result) break;
    }
    size_t minimum_internal = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t minimum_external = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
    heap_caps_monitor_local_minimum_free_size_stop();
    size_t internal_peak = before_internal > minimum_internal ? before_internal - minimum_internal : 0;
    size_t external_peak = before_external > minimum_external ? before_external - minimum_external : 0;
    size_t pixels = (f->width >> f->scale) * (f->height >> f->scale);
    unsigned checksum = 2166136261u;
    double error = 0;
    for (size_t i = 0; !result && i < pixels * 2; ++i) checksum = (checksum ^ output[i]) * 16777619;
    if (!result && reference_ok)
        for (size_t i = 0; i < pixels; ++i) {
            unsigned a = output[2*i] | output[2*i+1] << 8;
            unsigned b = reference[2*i] | reference[2*i+1] << 8;
            error += abs((int)(a >> 11) - (int)(b >> 11)) * 8;
            error += abs((int)((a >> 5) & 63) - (int)((b >> 5) & 63)) * 4;
            error += abs((int)(a & 31) - (int)(b & 31)) * 8;
        }
    printf("JPEG_BENCH {\"fixture\":\"%s\",\"codec\":\"%s\",\"scale\":%u,"
           "\"status\":%d,\"mean_us\":%lld,\"internal_peak\":%u,\"external_peak\":%u,"
           "\"output_bytes\":%u,\"checksum\":%u,\"mean_rgb_error\":%.3f,\"leak_bytes\":%d}\n",
           f->name, kind == 0 ? "rom" : kind == 1 ? "esp_new_jpeg" : "esp_new_jpeg_block",
           f->scale, result, (long long)(elapsed / (result ? 1 : REPEATS)),
           (unsigned)internal_peak, (unsigned)external_peak, (unsigned)pixels * 2, checksum,
           error / (pixels * 3),
           (int)(before_internal - heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)) +
           (int)(before_external - heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    fflush(stdout);
}
void app_main(void) {
    vTaskDelay(pdMS_TO_TICKS(5000));
    static const fixture_t fixtures[] = {
        {"small", small, sizeof(small), 96, 64, 0},
        {"photo", photo, sizeof(photo), 320, 180, 0},
        {"aligned", aligned, sizeof(aligned), 320, 192, 0},
        {"aligned-half", aligned, sizeof(aligned), 320, 192, 1},
        {"large", large, sizeof(large), 640, 384, 0},
        {"large-quarter", large, sizeof(large), 640, 384, 2},
        {"gray", gray, sizeof(gray), 320, 180, 0},
    };
    unsigned char *output = heap_caps_aligned_alloc(64, OUTPUT_CAPACITY, MALLOC_CAP_SPIRAM);
    unsigned char *reference = heap_caps_aligned_alloc(64, OUTPUT_CAPACITY, MALLOC_CAP_SPIRAM);
    if (!output || !reference) abort();
    for (unsigned i = 0; i < sizeof(fixtures) / sizeof(fixtures[0]); ++i) {
        const fixture_t *f = &fixtures[i];
        unsigned char *input = heap_caps_malloc(f->bytes, MALLOC_CAP_SPIRAM);
        if (!input) abort();
        memcpy(input, f->input, f->bytes);
        int reference_ok = !decode_rom(f, input, reference);
        for (int kind = 0; kind < 3; ++kind) run(f, kind, input, output, reference, reference_ok);
        heap_caps_free(input);
    }
    heap_caps_free(output); heap_caps_free(reference);
#ifdef PXA_BLIT_BENCHMARK
    extern void blit_benchmark(void);
    blit_benchmark();
#endif
    puts("JPEG_BENCH_DONE");
    fflush(stdout);
}
