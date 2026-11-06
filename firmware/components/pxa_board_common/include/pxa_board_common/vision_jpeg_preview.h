// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <pxa/pxa_esp_resource_memory.h>
#include <pxa/image/codecs.h>
#include <pxa/raster_assets.h>
#include <pxa_ext/vision_frame.h>
#include <algorithm>

// JPEG cameras share the native decoder and bounded immutable RGB565 output.
// Called only by the vision worker, including for its cached recording frame.
inline pxa_status_t PxaBoardVisionPreviewJpeg(void*, const uint8_t* bytes,
    size_t size, pxa_raster_asset_t** output) {
    if (!output) return PXA_STATUS_INVALID_ARGUMENT;
    *output = nullptr;
    if (!bytes || !size || size > PXA_VISION_MAX_JPEG) return PXA_STATUS_INVALID_ARGUMENT;
    pxa_image_decode_options_t options{};
    options.max_width = options.max_height = 320;
    options.max_pixels = 102400;
    options.max_encoded_bytes = PXA_VISION_MAX_JPEG;
    options.formats = 2; // JPEG
    options.opaque_rgb565 = 1;
    pxa_image_decoder_t* decoder = nullptr;
    auto status = pxa_image_decoder_create(&options,
        pxa_esp_device_resource_allocator(PXA_MEMORY_EXTERNAL, PXA_MEMORY_IMAGE),
        pxa_esp_device_resource_allocator(PXA_MEMORY_EXTERNAL, PXA_MEMORY_TEMPORARY),
        pxa_esp_rom_image_backend(), &decoder);
    size_t position = 0;
    while (status == PXA_STATUS_OK && position < size) {
        const auto count = std::min(size-position, pxa_image_decoder_input_budget(decoder));
        if (!count) { status = PXA_STATUS_PROTOCOL_ERROR; break; }
        status = pxa_image_decoder_write(decoder, {bytes+position, count});
        position += count;
    }
    pxa_decoded_image_t decoded{};
    if (status == PXA_STATUS_OK) status = pxa_image_decoder_finish(decoder, &decoded);
    pxa_image_decoder_destroy(decoder);
    if (status == PXA_STATUS_OK) *output = decoded.image;
    else pxa_asset_object_release(decoded.image);
    return status;
}
