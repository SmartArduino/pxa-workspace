// SPDX-License-Identifier: Apache-2.0
#include <cstdint>
#include <cstring>
#include "esp_async_color_convert.h"
#include "esp_memory_utils.h"
#include "sdkconfig.h"

// The synchronous Video read owns this Guest range until the transfer finishes.
// DMA2D retains the destination stride, including letterbox and caller padding.
extern "C" bool mosaico_video_copy_rows(uint8_t* out, uint32_t stride, const uint8_t* in,
                                       uint32_t width, uint32_t height) {
    constexpr unsigned cache_line = CONFIG_CACHE_L1_DCACHE_LINE_SIZE;
    const size_t row_bytes = size_t(width) * 2;
    if (row_bytes < cache_line || height < 4 || height > 0x3fff || stride / 2 > 0x3fff ||
        row_bytes * height < 32 * 1024 || ((uintptr_t(out) | uintptr_t(in) | stride) & 1u) ||
        !esp_ptr_external_ram(out) || !esp_ptr_external_ram(in)) return false;
    static const auto engine = [] {
        async_color_convert_handle_t handle = nullptr;
        async_color_convert_config_t config{};
        config.backlog = 1;
        config.dma_burst_size = 64;
        esp_async_color_convert_install_dma2d(&config, &handle);
        return handle;
    }();
    if (!engine) return false;

    // Keep both outside cache lines out of the driver's invalidate range.
    // Even an unaligned caller can have unrelated live data before/after it.
    const unsigned first = (uintptr_t(out) % cache_line) != 0;
    const unsigned last = ((uintptr_t(out) + size_t(height-1)*stride + row_bytes) % cache_line) != 0;
    async_color_convert_request_t request{};
    request.src_buffer = in + first * row_bytes;
    request.dst_buffer = out + first * stride;
    request.src_stride = request.copy_width = width;
    request.dst_stride = stride / 2;
    request.src_height = request.dst_height = request.copy_height = height - first - last;
    request.src_color_format = request.dst_color_format = ESP_COLOR_FOURCC_RGB16;
    if (esp_color_convert_blocking(engine, &request, -1) != ESP_OK) return false;
    if (first) std::memcpy(out, in, row_bytes);
    if (last) std::memcpy(out + size_t(height-1)*stride, in + size_t(height-1)*row_bytes, row_bytes);
    return true;
}
