// SPDX-License-Identifier: Apache-2.0
#include <pxa_board_common/vision_jpeg_preview.h>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>

struct pxa_image_decoder { size_t bytes = 0; };
static bool deny_create, deny_write, deny_finish, zero_budget;
static unsigned decoders, images;
static const pxa_memory_allocator_t allocator{};
static const pxa_image_backend_t backend{};
extern "C" const pxa_memory_allocator_t* pxa_esp_device_resource_allocator(unsigned memory, unsigned kind) {
    assert(memory == PXA_MEMORY_EXTERNAL);
    assert(kind == PXA_MEMORY_IMAGE || kind == PXA_MEMORY_TEMPORARY);
    return &allocator;
}
extern "C" const pxa_image_backend_t* pxa_esp_rom_image_backend() { return &backend; }
extern "C" pxa_status_t pxa_image_decoder_create(const pxa_image_decode_options_t* options,
    const pxa_memory_allocator_t* pixels, const pxa_memory_allocator_t* temporary,
    const pxa_image_backend_t* selected, pxa_image_decoder_t** output) {
    assert(options->max_width == 320 && options->max_height == 320);
    assert(options->max_pixels == 102400 && options->max_encoded_bytes == PXA_VISION_MAX_JPEG);
    assert(options->formats == 2 && options->opaque_rgb565 == 1);
    assert(pixels == &allocator && temporary == &allocator && selected == &backend);
    if (deny_create) return PXA_STATUS_RESOURCE_LIMIT;
    *output = new pxa_image_decoder;
    ++decoders;
    return PXA_STATUS_OK;
}
extern "C" size_t pxa_image_decoder_input_budget(const pxa_image_decoder_t*) { return zero_budget ? 0 : 3; }
extern "C" pxa_status_t pxa_image_decoder_write(pxa_image_decoder_t* decoder, pxa_bytes_t bytes) {
    assert(bytes.size > 0 && bytes.size <= 3);
    if (deny_write) return PXA_STATUS_PROTOCOL_ERROR;
    decoder->bytes += bytes.size;
    return PXA_STATUS_OK;
}
extern "C" pxa_status_t pxa_image_decoder_finish(pxa_image_decoder_t* decoder, pxa_decoded_image_t* output) {
    assert(decoder->bytes == 8);
    output->image = static_cast<pxa_asset_object_t*>(std::malloc(16));
    ++images;
    return deny_finish ? PXA_STATUS_PROTOCOL_ERROR : PXA_STATUS_OK;
}
extern "C" void pxa_image_decoder_destroy(pxa_image_decoder_t* decoder) {
    if (decoder) { --decoders; delete decoder; }
}
extern "C" void pxa_asset_object_release(pxa_asset_object_t* image) {
    if (image) { --images; std::free(image); }
}
int main() {
    std::array<uint8_t,8> bytes{};
    pxa_raster_asset_t* output = nullptr;
    assert(PxaBoardVisionPreviewJpeg(nullptr, bytes.data(), bytes.size(), nullptr) == PXA_STATUS_INVALID_ARGUMENT);
    assert(PxaBoardVisionPreviewJpeg(nullptr, nullptr, bytes.size(), &output) == PXA_STATUS_INVALID_ARGUMENT);
    assert(PxaBoardVisionPreviewJpeg(nullptr, bytes.data(), 0, &output) == PXA_STATUS_INVALID_ARGUMENT);
    assert(PxaBoardVisionPreviewJpeg(nullptr, bytes.data(), PXA_VISION_MAX_JPEG+1, &output) == PXA_STATUS_INVALID_ARGUMENT);
    for (bool* failure : {&deny_create, &deny_write, &deny_finish, &zero_budget}) {
        *failure = true;
        assert(PxaBoardVisionPreviewJpeg(nullptr, bytes.data(), bytes.size(), &output) != PXA_STATUS_OK);
        assert(!output && !images && !decoders);
        *failure = false;
    }
    assert(PxaBoardVisionPreviewJpeg(nullptr, bytes.data(), bytes.size(), &output) == PXA_STATUS_OK);
    assert(output && images == 1 && !decoders);
    pxa_raster_asset_release(output);
    assert(!images);
    puts("Vision JPEG preview: bounded RGB565, fragmented input, errors and immutable image ownership passed");
}
