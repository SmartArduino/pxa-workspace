// SPDX-License-Identifier: Apache-2.0
#include "pxa/pxa_esp_video.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static pxa_status_t open_decoder(void *ctx, uint16_t index, uint16_t w, uint16_t h, void **decoder) {
    (void)ctx; (void)index; (void)w; (void)h; (void)decoder;
    return PXA_STATUS_UNSUPPORTED;
}
static pxa_status_t decode(void *ctx, const uint8_t *p, size_t n, uint64_t pts, uint32_t flags,
                           pxa_video_frame_t *frame) {
    (void)ctx; (void)p; (void)n; (void)pts; (void)flags; (void)frame;
    return PXA_STATUS_UNSUPPORTED;
}
static pxa_status_t reset(void *ctx) { (void)ctx; return PXA_STATUS_OK; }
static void close_decoder(void *ctx) { (void)ctx; }

int main(int argc, char **argv) {
    const size_t sizes[] = {PXA_VIDEO_BACKEND_SYNC_SIZE, PXA_VIDEO_BACKEND_ASYNC_SIZE,
        PXA_VIDEO_BACKEND_DEFERRED_SIZE, PXA_VIDEO_BACKEND_SURFACE_SIZE,
        PXA_VIDEO_BACKEND_PREFERRED_SIZE, sizeof(pxa_video_backend_t), sizeof(pxa_video_backend_t)+16};
    assert(argc == 2);
    const unsigned i = (unsigned)atoi(argv[1]);
    assert(i < sizeof(sizes)/sizeof(sizes[0]));
    const pxa_video_capability_t caps = {0};
    pxa_video_backend_t original = {.count=1, .capabilities=&caps, .open=open_decoder,
        .decode=decode, .reset=reset, .close=close_decoder};
    const size_t available = sizes[i];
    const size_t known = available < sizeof(original) ? available : sizeof(original);
    void *provider = calloc(1, available);
    assert(provider && !pxa_esp_video_backend());
    original.struct_size = (uint32_t)available;
    memcpy(provider, &original, known);
    assert(pxa_esp_video_set_backend(provider));
    free(provider);
    const pxa_video_backend_t *registered = pxa_esp_video_backend();
    assert(registered && registered->struct_size == known && registered->open == open_decoder);
    assert(!registered->prepare_and_queue && !pxa_esp_video_set_backend(&original));
    return 0;
}
