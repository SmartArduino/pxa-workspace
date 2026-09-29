#undef NDEBUG
#include "pxa_codec_memory.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

static void *allocate(void *, size_t bytes) { return std::malloc(bytes); }
static void release(void *, void *memory) { std::free(memory); }
static void lock(void *p) { static_cast<std::mutex *>(p)->lock(); }
static void unlock(void *p) { static_cast<std::mutex *>(p)->unlock(); }

int main() {
    std::mutex mutex;
    pxa_memory_budget_t budget;
    pxa_memory_budget_config_t config = {};
    config.limit[0] = config.limit[1] = 8192;
    config.temporary_limit[1] = 1536;
    config.lock_context = &mutex; config.lock = lock; config.unlock = unlock;
    assert(!pxa_memory_budget_init(&budget, &config));
    const size_t limits[] = {4096, 4096};
    pxa_memory_allocator_t a[2] = {};
    for (auto &allocator : a) {
        assert(!pxa_memory_owner_open(&budget, limits, &allocator.owner));
        allocator.budget = &budget; allocator.memory_class = PXA_MEMORY_EXTERNAL;
        allocator.kind = PXA_MEMORY_TEMPORARY;
        allocator.allocate = allocate; allocator.release = release;
    }
    assert(!media_lib_module_malloc("outside scope", 1));
    void *memory;
    {
        PxaCodecMemoryScope scope(&a[0]);
        assert(!media_lib_module_calloc("overflow", SIZE_MAX, 2));
        assert(!media_lib_module_calloc("zero", 0, 123));
        memory = media_lib_module_calloc("test", 16, 16); assert(memory);
        for (unsigned i = 0; i < 256; ++i) assert(static_cast<unsigned char *>(memory)[i] == 0);
        std::memset(memory, 42, 256);
        {
            PxaCodecMemoryScope nested(&a[1]);
            assert(!media_lib_module_realloc("wrong owner", memory, 512));
            void *other = media_lib_module_malloc("other", 128); assert(other);
            media_lib_free(other);
        }
        memory = media_lib_module_realloc("grow", memory, 512); assert(memory);
        assert(static_cast<unsigned char *>(memory)[255] == 42);
        assert(!media_lib_module_realloc("quota", memory, 4096));
        assert(static_cast<unsigned char *>(memory)[255] == 42);
    }
    assert(!media_lib_module_malloc("restored", 1));
    {
        PxaCodecMemoryScope scope(&a[1]);
        // Fits both owner/global total limits, but not the global temporary
        // ceiling while the first owner's 512-byte block is still alive.
        assert(!media_lib_module_malloc("temporary cap", 1024));
        pxa_memory_stats_t stats;
        assert(!pxa_memory_budget_stats(&budget, 0, &stats));
        assert(stats.by_kind[PXA_MEMORY_TEMPORARY][1] == pxa_memory_allocation_bytes(512));
        assert(stats.temporary_peak[1] <= config.temporary_limit[1]);
    }
    // Free does not consult current task/owner. The old owner remains charged
    // and cannot be recycled until the last block is really freed.
    assert(pxa_memory_owner_close(&budget, a[0].owner) == PXA_STATUS_WOULD_BLOCK);
    std::thread late_free([&] { media_lib_free(memory); }); late_free.join();
    assert(!pxa_memory_owner_close(&budget, a[0].owner));
    assert(!pxa_memory_owner_open(&budget, limits, &a[0].owner));
    auto work = [](const pxa_memory_allocator_t *allocator) {
        assert(!media_lib_module_malloc("task has no scope", 1));
        PxaCodecMemoryScope scope(allocator);
        for (unsigned i = 0; i < 10000; ++i) {
            void *p = media_lib_module_malloc("concurrent", 128); assert(p);
            p = media_lib_module_realloc("concurrent", p, 256); assert(p);
            media_lib_free(p);
        }
    };
    std::thread one(work, &a[0]), two(work, &a[1]); one.join(); two.join();
    for (const auto &allocator : a) {
        pxa_memory_stats_t stats;
        assert(!pxa_memory_budget_stats(&budget, allocator.owner, &stats));
        assert(stats.charged[1] == 0 && stats.peak[1] >=
            pxa_memory_allocation_bytes(128) + pxa_memory_allocation_bytes(256));
        assert(!pxa_memory_owner_close(&budget, allocator.owner));
    }
    puts("codec allocation hooks: overflow, total/temporary quotas, realloc rollback, nested scopes, late free and task isolation passed");
}
