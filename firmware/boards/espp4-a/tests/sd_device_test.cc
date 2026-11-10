// SPDX-License-Identifier: Apache-2.0
#include "espp4_a_sd.h"
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_rom_crc.h>
#include <esp_timer.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <fcntl.h>

#ifndef PXA_ESPP4_SD_TEST_FORCE_INTERNAL
#define PXA_ESPP4_SD_TEST_FORCE_INTERNAL 0
#endif

bool espp4_a_sd_device_test(const char* mount) {
    constexpr size_t kBuffer = 64 * 1024, kBytes = 4 * 1024 * 1024 + 37;
    uint8_t* buffer = nullptr;
#if !PXA_ESPP4_SD_TEST_FORCE_INTERNAL
    buffer = static_cast<uint8_t*>(heap_caps_aligned_alloc(128, kBuffer + 128,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
#endif
    const bool external = buffer != nullptr;
    if (!buffer) buffer = static_cast<uint8_t*>(heap_caps_aligned_alloc(128, kBuffer + 128,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
    if (!buffer) { ESP_LOGE("espp4_sdtest", "DMA allocation failed"); return false; }
    ESP_LOGI("espp4_sdtest", "DMA test buffer: %s", external ? "PSRAM" : "internal SRAM");
    std::memset(buffer + kBuffer, 0xa5, 128);
    char directory[192], path[224];
    std::snprintf(directory, sizeof(directory), "%s/pxa-sd-selftest-%08lx", mount, (unsigned long)esp_random());
    if (mkdir(directory, 0700) != 0) {
        ESP_LOGE("espp4_sdtest", "mkdir failed: %s", std::strerror(errno));
        heap_caps_free(buffer); return false;
    }
    std::snprintf(path, sizeof(path), "%s/payload.bin", directory);
    int file = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    bool ok = file >= 0;
    uint32_t expected = 0, actual = 0, random = 0x4a202611;
    size_t written = 0, read = 0;
    const int64_t begin_write = esp_timer_get_time();
    for (size_t offset = 0; ok && offset < kBytes;) {
        const size_t bytes = std::min(kBuffer, kBytes - offset);
        for (size_t i = 0; i < bytes; i += 4) {
            random ^= random << 13; random ^= random >> 17; random ^= random << 5;
            std::memcpy(buffer + i, &random, std::min(size_t(4), bytes - i));
        }
        expected = esp_rom_crc32_le(expected, buffer, bytes);
        const ssize_t transferred = write(file, buffer, bytes);
        if (transferred > 0) written += size_t(transferred);
        ok = transferred == ssize_t(bytes);
        offset += bytes;
    }
    if (file >= 0) {
        if (fsync(file)) ok = false;
        if (close(file)) ok = false;
    }
    const int64_t write_us = esp_timer_get_time() - begin_write;
    const int64_t begin_read = esp_timer_get_time();
    file = ok ? open(path, O_RDONLY) : -1;
    if (file < 0) ok = false;
    for (size_t offset = 0; ok && offset < kBytes;) {
        const size_t bytes = std::min(kBuffer, kBytes - offset);
        const ssize_t transferred = ::read(file, buffer, bytes);
        if (transferred > 0) read += size_t(transferred);
        ok = transferred == ssize_t(bytes);
        if (ok) actual = esp_rom_crc32_le(actual, buffer, bytes);
        offset += bytes;
    }
    if (file >= 0) {
        uint8_t tail;
        if (::read(file, &tail, 1) != 0) ok = false;
        if (close(file)) ok = false;
    }
    const int64_t read_us = esp_timer_get_time() - begin_read;
    for (size_t i = 0; i < 128; ++i) if (buffer[kBuffer + i] != 0xa5) ok = false;
    ok = ok && actual == expected;
    ESP_LOGI("espp4_sdtest", "%s written=%u read=%u write_KiBs=%u read_KiBs=%u crc32=%08lx expected=%08lx",
        ok ? "PASS" : "FAIL", unsigned(written), unsigned(read),
        unsigned(written * 1000000ULL / (std::max(int64_t(1), write_us) * 1024)),
        unsigned(read * 1000000ULL / (std::max(int64_t(1), read_us) * 1024)),
        (unsigned long)actual, (unsigned long)expected);
    heap_caps_free(buffer);
    if (ok) { unlink(path); rmdir(directory); }
    else ESP_LOGW("espp4_sdtest", "Owned test file retained at %s", path);
    return ok;
}
