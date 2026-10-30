// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
// NAND bootstrap adapted from ESP-Mosaico BSP onboard/nand_flash.c,
// revision 7cbfb390638dcb93522fc3cde094b2d43cf907c2.
#include "esp_mosaico_storage.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <driver/gpio.h>
#include <diskio_bdl.h>
#include <diskio_impl.h>
#include <driver/spi_master.h>
#include <esp_blockdev.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_vfs_fat.h>
#include <spi_nand_flash.h>
#include <pxa/pxa_esp_storage.h>
#include <sdkconfig.h>

namespace {
constexpr char kTag[] = "mosaico_nand";
constexpr char kMount[] = CONFIG_PXA_MOUNT_POINT "/nand";
constexpr auto kHost = SPI3_HOST;
spi_device_handle_t g_device = nullptr;
esp_blockdev_handle_t g_wl = nullptr;
bool g_ready = false, g_bus_owned = false;

pxa_status_t QuerySpace(void*, const char* path, uint64_t* total, uint64_t* free_bytes, uint8_t* known) {
    *known = 0;
    if (esp_vfs_fat_info(path, total, free_bytes) != ESP_OK) return PXA_STATUS_IO_ERROR;
    *known = PXA_VOLUME_TOTAL_KNOWN | PXA_VOLUME_FREE_KNOWN;
    return PXA_STATUS_OK;
}

bool ReleaseDevice() {
    // This NAND BDL release consumes the handle even if it reports an error.
    if (g_wl) { (void)g_wl->ops->release(g_wl); g_wl = nullptr; }
    if (g_device) {
        if (spi_bus_remove_device(g_device) != ESP_OK) return false;
        g_device = nullptr;
    }
    if (g_bus_owned) {
        if (spi_bus_free(kHost) != ESP_OK) return false;
        g_bus_owned = false;
    }
    return true;
}

esp_err_t ReleaseControlPins() {
    constexpr gpio_num_t pins[] = {gpio_num_t(24), gpio_num_t(25)};
    for (const auto pin : pins) {
        // Before QE is set these are active-low HOLD and WP, not SPI data.
        auto error = gpio_set_level(pin, 1);
        if (error != ESP_OK) return error;
        gpio_config_t config = {};
        config.pin_bit_mask = uint64_t(1) << pin;
        config.mode = GPIO_MODE_OUTPUT;
        config.pull_up_en = GPIO_PULLUP_ENABLE;
        error = gpio_config(&config);
        if (error != ESP_OK) return error;
        error = gpio_set_level(pin, 1);
        if (error != ESP_OK) return error;
    }
    return ESP_OK;
}

esp_err_t InitializeSpi(bool quad) {
    spi_bus_config_t bus = {};
    bus.mosi_io_num = 21;
    bus.miso_io_num = 22;
    bus.sclk_io_num = 20;
    bus.quadhd_io_num = quad ? 24 : -1;
    bus.quadwp_io_num = quad ? 25 : -1;
    bus.data4_io_num = bus.data5_io_num = bus.data6_io_num = bus.data7_io_num = -1;
    bus.max_transfer_sz = 4096 + 64;
    auto error = spi_bus_initialize(kHost, &bus, SPI_DMA_CH_AUTO);
    if (error != ESP_OK) return error;
    g_bus_owned = true;
    spi_device_interface_config_t device = {};
    device.clock_speed_hz = 20000000;
    device.spics_io_num = 23;
    device.queue_size = 1;
    device.flags = SPI_DEVICE_HALFDUPLEX;
    return spi_bus_add_device(kHost, &device, &g_device);
}

esp_err_t BootstrapRead(uint8_t command, uint8_t address, uint8_t* data, size_t bytes) {
    spi_transaction_ext_t transfer = {};
    transfer.base.flags = SPI_TRANS_VARIABLE_ADDR | SPI_TRANS_VARIABLE_CMD |
        SPI_TRANS_VARIABLE_DUMMY | SPI_TRANS_USE_RXDATA;
    transfer.base.cmd = command;
    transfer.base.addr = address;
    transfer.base.rxlength = bytes * 8;
    transfer.address_bits = transfer.command_bits = 8;
    const auto error = spi_device_transmit(g_device, &transfer.base);
    if (error == ESP_OK) std::memcpy(data, transfer.base.rx_data, bytes);
    return error;
}

esp_err_t BootstrapQuad() {
    uint8_t id[2] = {};
    auto error = BootstrapRead(0x9f, 0, id, sizeof(id));
    if (error != ESP_OK) return error;
    ESP_LOGI(kTag, "NAND SPI ID manufacturer=0x%02x device=0x%02x", id[0], id[1]);
    switch (id[0]) {
    case 0xef: case 0x2c: // Winbond/Micron do not require a QE bit.
        return ESP_OK;
    case 0x52: case 0xc8: case 0xba: case 0x0b: case 0xa1: case 0xc2:
        break;
    default:
        return ESP_ERR_INVALID_RESPONSE;
    }
    uint8_t config = 0;
    error = BootstrapRead(0x0f, 0xb0, &config, 1);
    if (error != ESP_OK) return error;
    if (!(config & 1)) {
        spi_transaction_ext_t transfer = {};
        transfer.base.flags = SPI_TRANS_VARIABLE_ADDR | SPI_TRANS_VARIABLE_CMD |
            SPI_TRANS_VARIABLE_DUMMY | SPI_TRANS_USE_TXDATA;
        transfer.base.cmd = 0x1f;
        transfer.base.addr = 0xb0;
        transfer.base.length = 8;
        transfer.base.tx_data[0] = config | 1;
        transfer.address_bits = transfer.command_bits = 8;
        error = spi_device_transmit(g_device, &transfer.base);
        if (error != ESP_OK) return error;
        error = BootstrapRead(0x0f, 0xb0, &config, 1);
        if (error != ESP_OK) return error;
        if (!(config & 1)) return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGI(kTag, "NAND Quad enabled: configuration=0x%02x", config);
    return ESP_OK;
}

#ifdef CONFIG_PXA_MOSAICO_NAND_FORMAT_IF_MOUNT_FAILED
esp_err_t ProbeFilesystem(bool* missing, bool* detached) {
    *missing = false;
    *detached = true;
    BYTE drive = 0;
    auto error = ff_diskio_get_drive(&drive);
    if (error != ESP_OK) return error;
    auto* filesystem = static_cast<FATFS*>(std::calloc(1, sizeof(FATFS)));
    if (!filesystem) return ESP_ERR_NO_MEM;
    error = ff_diskio_register_bdl(drive, g_wl);
    if (error != ESP_OK) { std::free(filesystem); return error; }
    char name[8];
    std::snprintf(name, sizeof(name), "%u:", unsigned(drive));
    const auto result = f_mount(filesystem, name, 1);
    if (f_mount(nullptr, name, 0) != FR_OK) {
        // FatFS may still reference this allocation/BDL. Keep both alive;
        // initialization will remain unavailable until the next reboot.
        *detached = false;
        ESP_LOGW(kTag, "NAND filesystem probe could not detach");
        return ESP_FAIL;
    }
    ff_diskio_clear_pdrv_bdl(g_wl);
    ff_diskio_unregister(drive);
    std::free(filesystem);
    *missing = result == FR_NO_FILESYSTEM;
    ESP_LOGI(kTag, "NAND filesystem probe=%d; format-required=%d", int(result), int(*missing));
    return result == FR_OK || *missing ? ESP_OK : ESP_FAIL;
}
#endif
}

bool EspMosaicoInitializeStorage() {
    if (g_ready) return true;
    if (g_wl) return false; // A failed unmount still owns its live BDL.
    // Resume incomplete SPI rollback before trying to initialize that bus again.
    if (g_bus_owned && !ReleaseDevice()) return false;
    // Keep HOLD/WP high through identification and QE setup. Routing these
    // pins to SPI before QE can hold the chip and make READ ID return 0xff.
    auto error = ReleaseControlPins();
    if (error == ESP_OK) error = InitializeSpi(false);
    if (error == ESP_OK) error = BootstrapQuad();
    if (error != ESP_OK) {
        ESP_LOGW(kTag, "NAND single-line bootstrap: %s", esp_err_to_name(error));
        ReleaseDevice();
        return false;
    }
    if (!ReleaseDevice()) return false;
    error = InitializeSpi(true);
    if (error == ESP_OK) {
        spi_nand_flash_config_t nand = {};
        nand.device_handle = g_device;
        nand.io_mode = SPI_NAND_IO_MODE_QIO;
        nand.flags = SPI_DEVICE_HALFDUPLEX;
        error = spi_nand_flash_init_with_layers(&nand, &g_wl);
    }
    if (error != ESP_OK) {
        ESP_LOGW(kTag, "NAND detection/FTL: %s", esp_err_to_name(error));
        ReleaseDevice();
        return false;
    }
    esp_vfs_fat_mount_config_t mount = {};
#ifdef CONFIG_PXA_MOSAICO_NAND_FORMAT_IF_MOUNT_FAILED
    bool detached = true;
    error = ProbeFilesystem(&mount.format_if_mount_failed, &detached);
    if (error != ESP_OK) {
        ESP_LOGW(kTag, "NAND filesystem probe: %s", esp_err_to_name(error));
        if (detached) ReleaseDevice();
        return false;
    }
#endif
    mount.max_files = 16;
    mount.allocation_unit_size = 16 * 1024;
    error = esp_vfs_fat_bdl_mount(kMount, g_wl, &mount);
    if (error != ESP_OK) {
        ESP_LOGW(kTag, "Cannot mount %s: %s; format-on-failure is %s. "
                 "Check the board's automatic initialization setting for an unformatted chip.",
                 kMount, esp_err_to_name(error),
                 mount.format_if_mount_failed ? "enabled" : "disabled");
        ReleaseDevice();
        return false;
    }
    // The soldered chip has no portable card CID. Bind it to this MCU and
    // its fixed SPI wiring, independently of the current VFS mount path.
    uint8_t identity[32] = {};
    error = esp_efuse_mac_get_default(identity);
    std::memcpy(identity + 6, "mosaico-spi3-nand", 17);
    uint64_t total = 0, free_bytes = 0;
    const bool known = esp_vfs_fat_info(kMount, &total, &free_bytes) == ESP_OK;
    char drive[8];
    std::snprintf(drive, sizeof(drive), "%u:", unsigned(ff_diskio_get_pdrv_bdl(g_wl)));
    DWORD free_clusters = 0;
    FATFS* filesystem = nullptr;
    uint32_t allocation_unit = 0;
    if (f_getfree(drive, &free_clusters, &filesystem) == FR_OK && filesystem) {
#if FF_MAX_SS != FF_MIN_SS
        allocation_unit = uint32_t(filesystem->csize) * filesystem->ssize;
#else
        allocation_unit = uint32_t(filesystem->csize) * FF_MIN_SS;
#endif
    }
    pxa_esp_storage_volume_config_t config = {};
    config.struct_size = sizeof(config);
    config.mount_path = kMount; config.name = "NAND storage";
    config.device_identity = identity; config.device_identity_bytes = sizeof(identity);
    config.capabilities = PXA_VOLUME_READ | PXA_VOLUME_WRITE | PXA_VOLUME_RANDOM_READ |
        PXA_VOLUME_CASE_INSENSITIVE;
    if (allocation_unit) config.capabilities |= PXA_VOLUME_INSTALLABLE;
    config.allocation_unit = allocation_unit;
    config.max_file_bytes = sizeof(off_t) > 4 ? UINT32_MAX : INT32_MAX;
    config.total_bytes = total; config.free_bytes = free_bytes;
    config.space_known = known ? 3 : 0;
    config.link_free = 1; config.query_space = QuerySpace;
    uint64_t epoch = 0;
    const auto status = error == ESP_OK
        ? pxa_esp_storage_register_volume(&config, &epoch) : PXA_STATUS_UNAVAILABLE;
    if (status != PXA_STATUS_OK) {
        ESP_LOGW(kTag, "NAND volume registration: %ld", long(status));
        if (esp_vfs_fat_bdl_unmount(kMount, g_wl) != ESP_OK) {
            ESP_LOGW(kTag, "NAND unmount failed; retaining its SPI/BDL resources");
            return false;
        }
        ReleaseDevice();
        return false;
    }
    g_ready = true;
    const auto media = pxa_esp_storage_select_app_media(kMount);
    if (media != PXA_STATUS_OK) ESP_LOGW(kTag, "NAND media selection: %ld", long(media));
    ESP_LOGI(kTag, "NAND ready at %s: %llu MiB, SPI3 QIO 20 MHz, Dhara + FAT",
             kMount, static_cast<unsigned long long>(total / (1024 * 1024)));
    return true;
}
