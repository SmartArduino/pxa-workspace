// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <cstdio>
#include <esp_vfs_fat.h>
#include <diskio_sdmmc.h>
#include <sdmmc_cmd.h>
#include <pxa/pxa_esp_storage.h>

namespace pxa_board {
// Called once, after a successful FAT mount, before application activation.
// The card and VFS remain owned by the board for the lifetime of the mount.
inline pxa_status_t RegisterSdVolume(const char* mount, sdmmc_card_t* card) {
    char drive[8];
    std::snprintf(drive, sizeof(drive), "%u:", unsigned(ff_diskio_get_pdrv_card(card)));
    DWORD clusters = 0;
    FATFS* filesystem = nullptr;
    uint32_t unit = 0;
    if (f_getfree(drive, &clusters, &filesystem) == FR_OK && filesystem) {
#if FF_MAX_SS != FF_MIN_SS
        unit = uint32_t(filesystem->csize) * filesystem->ssize;
#else
        unit = uint32_t(filesystem->csize) * FF_MIN_SS;
#endif
    }
    uint64_t total = 0, free = 0;
    const bool known = esp_vfs_fat_info(mount, &total, &free) == ESP_OK;
    return pxa_esp_storage_register_fat(mount, "SD card", &card->cid,
        sizeof(card->cid), total, free, known ? 3 : 0, unit);
}
} // namespace pxa_board
