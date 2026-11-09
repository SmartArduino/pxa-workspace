// SPDX-License-Identifier: Apache-2.0
#include <sdmmc_cmd.h>
#include <esp_heap_caps.h>
#include <array>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <limits>
static int allocations, frees, calls, result;
static bool allocation_failure, aligned;
static const void* transferred;
static uint32_t reg;
void* heap_caps_aligned_alloc(size_t alignment, size_t size, unsigned caps) {
    assert(caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT) ||
           caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
    ++allocations;
    if (allocation_failure) return nullptr;
    void* p = nullptr;
    assert(posix_memalign(&p, alignment, size) == 0);
    return p;
}
void heap_caps_free(void* p) { ++frees; std::free(p); }
static int transfer(sdmmc_card_t* card, uint32_t function, uint32_t address, void* data, size_t size, bool write) {
    ++calls; transferred = data; reg = address;
    assert(function == 1 && size == 4096);
    if (card->host.slot == 1 && !aligned) assert(uintptr_t(data) % 128 == 0);
    if (write) for (size_t i = 0; i < size; ++i) assert(static_cast<uint8_t*>(data)[i] == 0x5a);
    else if (result == ESP_OK) std::memset(data, 0x3c, size);
    return result;
}
extern "C" {
#define DECLARE_READ(name) \
int __real_sdmmc_io_##name(sdmmc_card_t* c, uint32_t f, uint32_t a, void* p, size_t n) { return transfer(c,f,a,p,n,false); } \
int __wrap_sdmmc_io_##name(sdmmc_card_t*, uint32_t, uint32_t, void*, size_t);
#define DECLARE_WRITE(name) \
int __real_sdmmc_io_##name(sdmmc_card_t* c, uint32_t f, uint32_t a, const void* p, size_t n) { return transfer(c,f,a,const_cast<void*>(p),n,true); } \
int __wrap_sdmmc_io_##name(sdmmc_card_t*, uint32_t, uint32_t, const void*, size_t);
DECLARE_READ(read_bytes)
DECLARE_READ(read_blocks)
DECLARE_WRITE(write_bytes)
DECLARE_WRITE(write_blocks)
}
int main() {
    sdmmc_card_t card{{1, [](int, const void*, size_t) { return aligned; }}};
    std::array<uint8_t,4097> storage{};
    auto* data = storage.data()+1; // deliberately not cache aligned
    for (auto write : {__wrap_sdmmc_io_write_bytes, __wrap_sdmmc_io_write_blocks}) {
        std::memset(data,0x5a,4096);
        assert(write(&card,1,0x80001234,data,4096)==ESP_OK);
        assert(transferred != data && reg == 0x80001234);
    }
    for (auto read : {__wrap_sdmmc_io_read_bytes, __wrap_sdmmc_io_read_blocks}) {
        std::memset(data,0x5a,4096);
        assert(read(&card,1,0x80001234,data,4096)==ESP_OK);
        for (size_t i=0;i<4096;++i) assert(data[i]==0x3c);
        result=77;
        std::memset(data,0x5a,4096);
        assert(read(&card,1,0,data,4096)==77);
        for (size_t i=0;i<4096;++i) assert(data[i]==0x5a);
        result=ESP_OK;
    }
    assert(allocations==frees);
    const int allocated=allocations;
    aligned=true;
    assert(__wrap_sdmmc_io_read_blocks(&card,1,0,data,4096)==ESP_OK && transferred==data);
    card.host.slot=0; aligned=false;
    assert(__wrap_sdmmc_io_read_blocks(&card,1,0,data,4096)==ESP_OK && transferred==data);
    assert(allocations==allocated);
    card.host.slot=1; allocation_failure=true;
    const int called=calls;
    assert(__wrap_sdmmc_io_read_blocks(&card,1,0,data,4096)==ESP_ERR_NO_MEM && calls==called);
    assert(__wrap_sdmmc_io_read_bytes(nullptr,1,0,data,4096)==ESP_ERR_INVALID_ARG);
    assert(__wrap_sdmmc_io_read_bytes(&card,1,0,data,std::numeric_limits<size_t>::max())==ESP_ERR_INVALID_SIZE);
    assert(storage[0]==0);
    puts("ESPP4-A: SDIO DMA alignment, fixed address, data/error preservation and Slot 0 isolation passed");
}
