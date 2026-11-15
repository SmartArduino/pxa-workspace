// SPDX-License-Identifier: Apache-2.0
// Included only by the P4 camera translation unit, AFTER PpaImage.
// Opt-in diagnostic: it never consumes/writes a CSI frame or publishes pixels.
namespace camera_dma_image_selftest {
constexpr size_t kSourceBytes = 4u * 1024u * 1024u;
constexpr size_t kBytes = espp4_a::kCameraPpaPreviewBytes;
static_assert(kSensorBytes <= kSourceBytes);
static_assert(kBytes == 792960u);
struct Source {
    const bool portrait = camera.portrait;
    uint16_t* pixels = nullptr;
    ~Source() { camera.portrait = portrait; heap_caps_free(pixels); }
};
struct Images {
    pxa_raster_asset_t* value[2]{};
    ~Images() {
        pxa_raster_asset_release(value[1]);
        pxa_raster_asset_release(value[0]);
    }
};
pxa_status_t Fail(const char* reason) {
    ESP_LOGE(kTag, "DMA image self-test FAIL portrait=%u stage=%s", camera.portrait, reason);
    return PXA_STATUS_INTERNAL;
}
uint64_t Hash(const uint8_t* data, size_t bytes) {
    uint64_t result = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < bytes; ++i) {
        result ^= data[i]; result *= UINT64_C(1099511628211);
    }
    return result;
}
bool Nontrivial(const uint8_t* data) {
    // Uniform zero/black/stale data cannot satisfy this predicate.
    const auto* pixels = reinterpret_cast<const uint16_t*>(data);
    uint16_t distinct[4]{};
    unsigned count = 0;
    for (size_t i = 0; i < kBytes / 2; ++i) {
        unsigned j = 0;
        while (j < count && distinct[j] != pixels[i]) ++j;
        if (j == count) {
            distinct[count++] = pixels[i];
            if (count == 4) return true;
        }
    }
    return false;
}
void Fill(uint16_t* pixels, uint32_t nonce, unsigned pattern) {
    constexpr uint16_t colors[4] = {0xf800, 0x07e0, 0x001f, 0xffff};
    const uint16_t salt = uint16_t(nonce) ^ (pattern ? uint16_t(0x5aa5) : uint16_t(0));
    // Tail is our allocation too; initialize/flush it without touching CSI RAM.
    std::fill_n(pixels, kSourceBytes / 2, uint16_t(0xa55a) ^ salt);
    for (unsigned y = 0; y < kSensorHeight; ++y) {
        for (unsigned x = 0; x < kSensorWidth; ++x) {
            const unsigned quadrant = (y >= kSensorHeight / 2 ? 2u : 0u) |
                                      (x >= kSensorWidth / 2 ? 1u : 0u);
            const bool checker = ((x / 64u) ^ (y / 64u) ^ pattern) & 1u;
            pixels[size_t(y) * kSensorWidth + x] = colors[quadrant] ^ salt ^
                (checker ? uint16_t(0x0821) : uint16_t(0));
        }
    }
}
pxa_status_t CreateDirect(unsigned width, unsigned height,
                          pxa_raster_asset_t** out, uint8_t** payload) {
    pxa_asset_info_t info{};
    info.kind = PXA_ASSET_IMAGE; info.encoding = PXA_ASSET_ENCODING_RGB565;
    info.format_version = 1; info.payload_offset = PXA_ASSET_FILE_HEADER_BYTES;
    info.width = width; info.height = height;
    info.decoded_bytes = kBytes; info.stored_bytes = kBytes + PXA_ASSET_FILE_HEADER_BYTES;
    *out = nullptr; *payload = nullptr;
    for (unsigned offset = 0; offset != 2; ++offset) {
        const pxa_status_t status = pxa_asset_object_create(&info,
            espp4_a::CameraImageAllocate, espp4_a::CameraImageRelease,
            const_cast<espp4_a::CameraImageAllocator*>(&kImageAllocators[offset]), out, payload);
        if (status != PXA_STATUS_OK) return status;
        // Inspect the ACTUAL public payload. Never infer private Core layout.
        if (espp4_a::CameraImageDmaReady(*payload, kBytes, camera.cache_alignment))
            return PXA_STATUS_OK;
        pxa_raster_asset_release(*out); *out = nullptr; *payload = nullptr;
    }
    return PXA_STATUS_UNAVAILABLE;
}
bool View(const pxa_raster_asset_t* image, unsigned width, unsigned height,
          pxa_asset_object_view_t* view) {
    pxa_asset_object_view(image, view);
    return view->data && view->bytes == kBytes && view->width == width &&
        view->height == height && view->kind == PXA_ASSET_IMAGE &&
        view->encoding == PXA_ASSET_ENCODING_RGB565;
}
} // namespace camera_dma_image_selftest

pxa_status_t CameraDmaImageSelftest() {
    using namespace camera_dma_image_selftest;
    // Diagnostic success requires actual PPA staging AND direct Core targets.
    // No CPU transform or skipped path can count as a successful self-test.
    if (!camera.ppa || camera.ppa_failed || !camera.preview_dma ||
        !espp4_a::CameraImageDmaReady(camera.preview_dma, kBytes, camera.cache_alignment))
        return Fail("PPA/cache/staging unavailable");
    Source source;
    source.pixels = static_cast<uint16_t*>(heap_caps_aligned_alloc(
        espp4_a::kCameraImageDmaAlignment, kSourceBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!source.pixels) return PXA_STATUS_RESOURCE_LIMIT;
    const uint32_t nonce = uint32_t(esp_timer_get_time()) ^ UINT32_C(0x6d2b79f5);
    for (unsigned orientation = 0; orientation != 2; ++orientation) {
        Images images;
        camera.portrait = orientation != 0;
        const unsigned width = camera.portrait ? espp4_a::kCameraPpaPreviewHeight : espp4_a::kCameraPpaPreviewWidth;
        const unsigned height = camera.portrait ? espp4_a::kCameraPpaPreviewWidth : espp4_a::kCameraPpaPreviewHeight;
        uint64_t hashes[2]{};
        for (unsigned pattern = 0; pattern != 2; ++pattern) {
            Fill(source.pixels, nonce, pattern);
            esp_err_t error = esp_cache_msync(source.pixels, kSourceBytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
            if (error != ESP_OK) return Error("DMA self-test source cache", error);
            if (!PpaImage(source.pixels, camera.preview_dma, kBytes, width, height))
                return Fail("staging PPA");
            error = esp_cache_msync(camera.preview_dma, kBytes, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
            if (error != ESP_OK) return Error("DMA self-test staging cache", error);
            if (!Nontrivial(camera.preview_dma)) return Fail("staging colors");
            uint8_t* payload = nullptr;
            const pxa_status_t created = CreateDirect(width, height, &images.value[pattern], &payload);
            if (created != PXA_STATUS_OK) return created;
            if (!PpaImage(source.pixels, payload, kBytes, width, height))
                return Fail("direct PPA");
            error = esp_cache_msync(payload, kBytes, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
            if (error != ESP_OK) return Error("DMA self-test direct cache", error);
            if (std::memcmp(camera.preview_dma, payload, kBytes) != 0)
                return Fail("full staging/direct comparison");
            pxa_raster_asset_finish_loading(images.value[pattern]);
            pxa_asset_object_view_t view{};
            if (!View(images.value[pattern], width, height, &view) || view.data != payload ||
                !Nontrivial(view.data)) return Fail("public Core view/colors");
            hashes[pattern] = Hash(view.data, kBytes);
            if (!hashes[pattern] || hashes[pattern] == UINT64_MAX)
                return Fail("nontrivial checksum");
            if (!pattern) {
                // Retain an independent public consumer reference, then drop
                // the creator's reference. The first immutable image survives
                // the second staging/direct DMA and is never a DMA target again.
                pxa_raster_asset_retain(images.value[0]);
                pxa_raster_asset_release(images.value[0]);
            } else {
                pxa_asset_object_view_t first{};
                if (!View(images.value[0], width, height, &first) ||
                    Hash(first.data, kBytes) != hashes[0]) return Fail("retained first image changed");
                if (hashes[0] == hashes[1] || std::memcmp(first.data, view.data, kBytes) == 0)
                    return Fail("second pattern did not change");
            }
        }
        ESP_LOGI(kTag, "DMA image self-test PASS portrait=%u bytes=%u colors_min=4 first=%016llx second=%016llx retained=unchanged nonce=%08lx",
            camera.portrait, unsigned(kBytes), static_cast<unsigned long long>(hashes[0]),
            static_cast<unsigned long long>(hashes[1]), static_cast<unsigned long>(nonce));
    }
    return PXA_STATUS_OK;
}
