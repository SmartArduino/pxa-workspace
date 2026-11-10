// SPDX-License-Identifier: Apache-2.0
#include "espp4_a_sd.h"
#include "espp4_a_sd_bus.h"
#include <driver/sdmmc_host.h>
#include <sd_protocol_defs.h>
#include <algorithm>
#ifndef PXA_ESPP4_SD_SINGLE_BLOCK
#define PXA_ESPP4_SD_SINGLE_BLOCK 0
#endif
#ifdef ESP_PLATFORM
#include <esp_log.h>
#endif

extern "C" {
esp_err_t __real_sdmmc_read_sectors(sdmmc_card_t*, void*, size_t, size_t);
esp_err_t __real_sdmmc_write_sectors(sdmmc_card_t*, const void*, size_t, size_t);
}

namespace {
template<class Pointer, class Transfer>
esp_err_t TransferSectors(sdmmc_card_t* card, Pointer data, size_t start,
                         size_t count, Transfer transfer) {
    if (!count || !card || card->host.slot != SDMMC_HOST_SLOT_0)
        return transfer(card, data, start, count);
    // The current legacy host allocates four 4 KiB DMA descriptors. Keep each
    // command inside that initial window, avoiding interrupt-time ring refills
    // for large direct PSRAM buffers (which bypass IDF's buffer chunking).
    constexpr size_t kMaxBytes = 16 * 1024;
    const size_t sector = card->csd.sector_size;
    if (!data || !sector || sector > kMaxBytes) return ESP_ERR_INVALID_ARG;
    // A diagnostic switch can isolate multi-block protocol failures. Normal
    // operation uses the schematic's four data lines and multi-block DMA.
    const size_t max_blocks = PXA_ESPP4_SD_SINGLE_BLOCK ? 1 : kMaxBytes / sector;
    // Reject the entire invalid range before writing any of it.
    if (start > card->csd.capacity || count > card->csd.capacity - start)
        return ESP_ERR_INVALID_SIZE;
    while (count) {
        const size_t blocks = std::min(count, max_blocks);
        esp_err_t result;
        { espp4_a::SdBusGuard guard; result = transfer(card, data, start, blocks); }
        if (result != ESP_OK) return result;
        data += blocks * sector;
        start += blocks;
        count -= blocks;
    }
    return ESP_OK;
}
}

extern "C" esp_err_t __wrap_sdmmc_read_sectors(sdmmc_card_t* card, void* data,
                                              size_t start, size_t count) {
    return TransferSectors(card, static_cast<uint8_t*>(data), start, count, __real_sdmmc_read_sectors);
}
extern "C" esp_err_t __wrap_sdmmc_write_sectors(sdmmc_card_t* card, const void* data,
                                               size_t start, size_t count) {
    return TransferSectors(card, static_cast<const uint8_t*>(data), start, count, __real_sdmmc_write_sectors);
}

esp_err_t espp4_a_sd_transaction(int slot, sdmmc_command_t* command) {
    espp4_a::SdBusGuard guard;
    // IDF's block write uses R1, then polls CMD13 in a separate transaction.
    // Between those transactions SDIO can select Slot 1 and change the shared
    // clock while Slot 0 still asserts DAT0 busy. SCF_WAIT_BUSY keeps IDF's
    // controller mutex held through programming, using its existing timeout.
    // Do not wait for SDIO writes: their DAT0 has a different meaning.
    const auto flags = command ? command->flags : 0;
    if (slot == SDMMC_HOST_SLOT_0 && command &&
        (command->opcode == MMC_WRITE_BLOCK_SINGLE || command->opcode == MMC_WRITE_BLOCK_MULTIPLE))
        command->flags |= SCF_WAIT_BUSY;
    const auto result = sdmmc_host_do_transaction(slot, command);
#ifdef ESP_PLATFORM
#ifdef PXA_ESPP4_SD_SELFTEST
    if (command && (command->opcode == 8 || command->opcode == 41))
        ESP_LOGI("espp4_sd", "CMD%u arg=%08lx response=%08lx status=%s",
            unsigned(command->opcode), (unsigned long)command->arg,
            (unsigned long)command->response[0], esp_err_to_name(result));
#endif
    // Memory cards normally reject the initial SDIO CMD5/CMD52 probes.
    if (slot == SDMMC_HOST_SLOT_0 && command && command->opcode != 5 && command->opcode != 52 &&
        (result != ESP_OK || command->error != ESP_OK))
        ESP_LOGE("espp4_sd", "slot=%d CMD%u bytes=%u failed: %s",
            slot, unsigned(command->opcode), unsigned(command->datalen),
            esp_err_to_name(result != ESP_OK ? result : command->error));
#endif
    if (command) command->flags = flags;
    return result;
}
