#include "sdkconfig.h"
#if CONFIG_PXADB_ENABLED && (CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32S31)
#include "pxadb_jpeg_encode.h"
#if CONFIG_PXADB_JPEG_PSRAM_WORKSPACE
#include "pxadb_jpeg_memory.h"
#endif
#include <esp_heap_caps.h>
#include <esp_jpeg_enc.h>

namespace pxadb {
/* JPEG capture encodes the captured logical RGB565 frame. The board adapter
 * already snapshots the final presented frame under its own rotation lock and
 * converts it to logical orientation, so the encoder never touches the live
 * scanout buffer and a mid-encode display update cannot tear the image.
 *
 * The encoder is opened and closed per capture so it stays non-resident: an
 * idle device keeps none of its ~10 KiB DRAM state or the output buffer.
 * task_enable=false also means no encoder task exists at any point. */
constexpr uint8_t kJpegQuality = 70;

bool EncodeCaptureJpeg(const uint16_t* pixels, const TestControlCaptureInfo& info,
                     uint8_t** output, size_t* output_size,
                     JpegEncodeStats* stats) {
    if (pixels == nullptr || output == nullptr || output_size == nullptr ||
        info.stride_bytes != info.width * sizeof(uint16_t))
        return false;
    jpeg_enc_config_t config = DEFAULT_JPEG_ENC_CONFIG();
    config.width = static_cast<int>(info.width);
    config.height = static_cast<int>(info.height);
    config.src_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    config.subsampling = JPEG_SUBSAMPLE_420;
    config.quality = kJpegQuality;
    config.rotate = JPEG_ROTATE_0D;
    config.task_enable = false;
    const uint64_t pixel_count = static_cast<uint64_t>(info.width) * info.height;
    if (pixel_count == 0 || pixel_count > INT32_MAX / 4) return false;
    const size_t raw_bytes = static_cast<size_t>(pixel_count) * 2;
    // Keep the original conservative capacity. The S3 vendor encoder can
    // corrupt the heap with an undersized output instead of failing safely;
    // a small-buffer/retry strategy is therefore not supported here.
    const size_t capacity = raw_bytes * 2;
#if CONFIG_PXADB_JPEG_PSRAM_WORKSPACE
    JpegMemoryScope memory_scope;
#endif
    if (stats) *stats = {capacity, 1};
    jpeg_enc_handle_t handle = nullptr;
    if (jpeg_enc_open(&config, &handle) != JPEG_ERR_OK || handle == nullptr)
        return false;
    auto* encoded = static_cast<uint8_t*>(
        heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (encoded == nullptr) {
        jpeg_enc_close(handle);
        return false;
    }
    int encoded_size = 0;
    const jpeg_error_t result = jpeg_enc_process(handle,
        reinterpret_cast<const uint8_t*>(pixels), static_cast<int>(raw_bytes),
        encoded, static_cast<int>(capacity), &encoded_size);
    jpeg_enc_close(handle);
    if (result == JPEG_ERR_OK && encoded_size > 0 &&
        static_cast<size_t>(encoded_size) <= capacity) {
        *output = encoded;
        *output_size = static_cast<size_t>(encoded_size);
        return true;
    }
    heap_caps_free(encoded);
    return false;
}

bool MakeJpegSelfTest(uint8_t** output, size_t* output_size, JpegEncodeStats* stats) {
    TestControlCaptureInfo info;
    info.width = info.height = 256;
    info.stride_bytes = 512;
    auto* pixels = static_cast<uint16_t*>(heap_caps_aligned_calloc(
        16, 256 * 256, sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!pixels) return false;
    uint32_t seed = 0x3c6ef372;
    for (size_t i = 0; i < 256 * 256; ++i) {
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        pixels[i] = static_cast<uint16_t>(seed);
    }
    const bool success = EncodeCaptureJpeg(pixels, info, output, output_size, stats);
    heap_caps_free(pixels);
    return success;
}

} // namespace pxadb
#endif
