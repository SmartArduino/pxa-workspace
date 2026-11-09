// SPDX-License-Identifier: Apache-2.0
#include <sdmmc_cmd.h>
#include <esp_heap_caps.h>
#include <cstring>

// Current IDF requires cache-aligned DMA buffers for SDIO block transfers.
// The supplier's unchanged driver uses ordinary malloc. Adapt only the public
// SDMMC API boundary for its Slot 1; the storage card on Slot 0 is unaffected.
// Byte counts, address increment/fixed-address flags and errors stay intact.
extern "C" {
esp_err_t __real_sdmmc_io_read_bytes(sdmmc_card_t*, uint32_t, uint32_t, void*, size_t);
esp_err_t __real_sdmmc_io_write_bytes(sdmmc_card_t*, uint32_t, uint32_t, const void*, size_t);
esp_err_t __real_sdmmc_io_read_blocks(sdmmc_card_t*, uint32_t, uint32_t, void*, size_t);
esp_err_t __real_sdmmc_io_write_blocks(sdmmc_card_t*, uint32_t, uint32_t, const void*, size_t);
}
namespace {
template<bool Write, class Transfer>
esp_err_t TransferRadio(sdmmc_card_t* card, uint32_t function, uint32_t address,
                       void* data, size_t size, Transfer transfer) {
    if (!card || !data || !size) return ESP_ERR_INVALID_ARG;
    const size_t aligned_size = (size + 3u) & ~size_t(3);
    if (aligned_size < size) return ESP_ERR_INVALID_SIZE;
    if (card->host.slot != 1 ||
        card->host.check_buffer_alignment(card->host.slot, data, aligned_size))
        return transfer(card, function, address, data, size);
    // P4 SDMMC can DMA cache-aligned PSRAM. Large radio reads must not compete
    // with TLS, task stacks and DMA descriptors for scarce internal SRAM.
    // Ask the actual host before using external memory; byte transfers that
    // do not span complete cache lines keep the original internal fallback.
    void* bounce = nullptr;
    if (aligned_size >= 512 && aligned_size % 128 == 0) {
        bounce = heap_caps_aligned_alloc(128, aligned_size,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
        if (bounce && !card->host.check_buffer_alignment(card->host.slot, bounce, aligned_size)) {
            heap_caps_free(bounce);
            bounce = nullptr;
        }
    }
    if (!bounce) bounce = heap_caps_aligned_alloc(128, aligned_size,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!bounce) return ESP_ERR_NO_MEM;
    if constexpr (Write) std::memcpy(bounce, data, size);
    const auto result = transfer(card, function, address, bounce, size);
    if constexpr (!Write) if (result == ESP_OK) std::memcpy(data, bounce, size);
    heap_caps_free(bounce);
    return result;
}
}
extern "C" esp_err_t __wrap_sdmmc_io_read_bytes(sdmmc_card_t* c, uint32_t f, uint32_t a, void* p, size_t n) {
    return TransferRadio<false>(c, f, a, p, n, __real_sdmmc_io_read_bytes);
}
extern "C" esp_err_t __wrap_sdmmc_io_write_bytes(sdmmc_card_t* c, uint32_t f, uint32_t a, const void* p, size_t n) {
    return TransferRadio<true>(c, f, a, const_cast<void*>(p), n, __real_sdmmc_io_write_bytes);
}
extern "C" esp_err_t __wrap_sdmmc_io_read_blocks(sdmmc_card_t* c, uint32_t f, uint32_t a, void* p, size_t n) {
    return TransferRadio<false>(c, f, a, p, n, __real_sdmmc_io_read_blocks);
}
extern "C" esp_err_t __wrap_sdmmc_io_write_blocks(sdmmc_card_t* c, uint32_t f, uint32_t a, const void* p, size_t n) {
    return TransferRadio<true>(c, f, a, const_cast<void*>(p), n, __real_sdmmc_io_write_blocks);
}
