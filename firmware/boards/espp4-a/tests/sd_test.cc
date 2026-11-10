// SPDX-License-Identifier: Apache-2.0
#include "espp4_a_sd.h"
#include <cassert>
#include <cstdio>
#include <initializer_list>
#include <array>
#include <limits>
#include <vector>

struct SectorCall { const void* data; size_t start, count; };
static std::vector<SectorCall> sectors;
static size_t fail_at = std::numeric_limits<size_t>::max();
extern "C" {
esp_err_t __real_sdmmc_write_sectors(sdmmc_card_t*, const void* data, size_t start, size_t count) {
    sectors.push_back({data, start, count});
    return sectors.size() == fail_at ? 77 : ESP_OK;
}
esp_err_t __real_sdmmc_read_sectors(sdmmc_card_t* card, void* data, size_t start, size_t count) {
    return __real_sdmmc_write_sectors(card, data, start, count);
}
esp_err_t __wrap_sdmmc_read_sectors(sdmmc_card_t*, void*, size_t, size_t);
esp_err_t __wrap_sdmmc_write_sectors(sdmmc_card_t*, const void*, size_t, size_t);
}

static int expected_slot, expected_flags, returned;
static sdmmc_command_t* expected_command;
esp_err_t sdmmc_host_do_transaction(int slot, sdmmc_command_t* command) {
    assert(slot == expected_slot && command == expected_command);
    if (command) {
        assert(command->flags == expected_flags);
        assert(command->timeout_ms == 5000); // Do not hide timeouts.
        command->error = 73;
    }
    return returned;
}
int main() {
    for (int slot : {0, 1}) for (int opcode : {17, 18, 24, 25, 52, 53})
        for (int flags : {0x45, 0x45 | SCF_WAIT_BUSY}) for (int result : {0, 77}) {
            sdmmc_command_t command{opcode, flags, 5000, 0};
            expected_command = &command;
            expected_slot = slot;
            expected_flags = flags | ((slot == 0 && (opcode == 24 || opcode == 25)) ? SCF_WAIT_BUSY : 0);
            returned = result;
            assert(espp4_a_sd_transaction(slot, &command) == result);
            assert(command.flags == flags && command.error == 73);
        }
    expected_command = nullptr; returned = 1;
    assert(espp4_a_sd_transaction(expected_slot, nullptr) == returned);
    sdmmc_card_t card{{0, nullptr}};
    std::array<uint8_t, 65536 + 512> data{};
    for (auto count : {size_t(0), size_t(1), size_t(32), size_t(33), size_t(128), size_t(129)}) {
        sectors.clear();
        assert(__wrap_sdmmc_write_sectors(&card, data.data(), 19, count) == ESP_OK);
        size_t transferred = 0;
        for (const auto& call : sectors) {
            assert(call.start == 19 + transferred && call.count <= 32);
            assert(call.data == data.data() + transferred * 512);
            transferred += call.count;
        }
        assert(transferred == count);
    }
    sectors.clear(); fail_at = 2;
    assert(__wrap_sdmmc_read_sectors(&card, data.data(), 0, 128) == 77);
    assert(sectors.size() == 2); // Never continue after a partial I/O failure.
    sectors.clear();
    assert(__wrap_sdmmc_write_sectors(&card, data.data(), 99999, 2) == ESP_ERR_INVALID_SIZE);
    assert(__wrap_sdmmc_write_sectors(&card, data.data(), std::numeric_limits<size_t>::max(), 2) == ESP_ERR_INVALID_SIZE);
    assert(sectors.empty());
    card.ocr = 0; fail_at = std::numeric_limits<size_t>::max();
    assert(__wrap_sdmmc_write_sectors(&card, data.data(), 19, 129) == ESP_OK);
    assert(sectors.size() == 5);
    for (size_t i = 0; i < sectors.size(); ++i) {
        assert(sectors[i].count == (i == 4 ? 1 : 32) && sectors[i].start == 19 + i * 32);
        assert(sectors[i].data == data.data() + i * 16384);
    }
    sectors.clear();
    assert(__wrap_sdmmc_read_sectors(&card, data.data(), 19, 129) == ESP_OK);
    assert(sectors.size() == 5 && sectors[0].count == 32);
    sectors.clear();
    card.host.slot = 1; fail_at = std::numeric_limits<size_t>::max();
    assert(__wrap_sdmmc_read_sectors(&card, data.data(), 0, 129) == ESP_OK);
    assert(sectors.size() == 1 && sectors[0].count == 129); // Radio is untouched.
    puts("ESPP4-A: TF writes wait for busy within the host transaction; SDIO, reads, timeouts and errors preserved");
    puts("ESPP4-A: bounded sector commands, exact offsets, complete range validation and partial-error stop passed");
}
