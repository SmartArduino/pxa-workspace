// SPDX-License-Identifier: Apache-2.0
#include <sdmmc_cmd.h>
#include <esp_heap_caps.h>
#include <array>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <limits>
#include "espp4_a_sd_bus.h"
#include <condition_variable>
#include <thread>
static std::mutex init_mutex;
static std::condition_variable init_condition;
static bool init_entered, init_release;
static unsigned init_calls;
static int probe_step, probe_result;
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
int __wrap_sdmmc_card_init(const sdmmc_host_t*, sdmmc_card_t*);
int __wrap_sdmmc_io_read_byte(sdmmc_card_t*, uint32_t, uint32_t, uint8_t*);
int __real_sdmmc_io_read_byte(sdmmc_card_t*, uint32_t, uint32_t, uint8_t* p) {
    if (p) *p=0x69;
    return ESP_OK;
}
int __real_sdmmc_io_write_byte(sdmmc_card_t*, uint32_t, uint32_t, uint8_t value, uint8_t* p) {
    if (p) *p=value;
    return ESP_OK;
}
int __real_sdmmc_card_init(const sdmmc_host_t*, sdmmc_card_t* card) {
    ++init_calls;
    uint8_t value=0;
    assert(__wrap_sdmmc_io_read_byte(card,0,0,&value)==ESP_OK && value==0x69);
    std::unique_lock lock(init_mutex);
    init_entered=true;init_condition.notify_all();
    init_condition.wait(lock,[]{return init_release;});
    return 77;
}
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
    std::thread initializing([&]{assert(__wrap_sdmmc_card_init(&card.host,&card)==77);});
    {
        std::unique_lock lock(init_mutex);
        init_condition.wait(lock,[]{return init_entered;});
    }
    const bool acquired=espp4_a::sd_bus_mutex.try_lock();
    if(acquired)espp4_a::sd_bus_mutex.unlock();
    assert(!acquired); // No second slot may interleave with card initialization.
    {std::lock_guard lock(init_mutex);init_release=true;}
    init_condition.notify_all();initializing.join();
    assert(espp4_a::sd_bus_mutex.try_lock());
    espp4_a::sd_bus_mutex.unlock();
    card.host.slot=0;
    card.host.set_card_clk=[](int slot,uint32_t khz) {
        assert(slot==0 && khz==400 && probe_step==0);probe_step=1;return probe_result;
    };
    card.host.get_real_freq=[](int slot,int* khz) {
        assert(slot==0 && probe_step==1);probe_step=2;*khz=400;return ESP_OK;
    };
    assert(__wrap_sdmmc_card_init(&card.host,&card)==77 && probe_step==2);
    const auto initialized=init_calls;
    probe_step=0;probe_result=99;
    assert(__wrap_sdmmc_card_init(&card.host,&card)==99 && init_calls==initialized);
    puts("ESPP4-A: SDIO DMA alignment, fixed address, data/error preservation and Slot 0 isolation passed");
    puts("ESPP4-A: recursive SD/SDIO protocol locking, initialization isolation and error-path release passed");
    puts("ESPP4-A: stable 400 kHz probing is configured before TF initialization; errors propagate");
}
