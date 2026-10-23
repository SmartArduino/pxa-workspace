#include <stdint.h>
#define API(n) __attribute__((export_name(#n), noinline))
API(load_any) uint32_t load_any(uint32_t a) {
    uint32_t v;
    __builtin_memcpy(&v, (void *)(uintptr_t)a, 4);
    return v;
}
API(load_aligned) uint32_t load_aligned(uint32_t a) { return *(uint32_t *)(uintptr_t)(a & ~3u); }
API(load_half) uint32_t load_half(uint32_t a) { return *(uint16_t *)(uintptr_t)(a & ~1u); }
API(load_offset) uint32_t load_offset(uint32_t a) {
    return *(uint32_t *)(uintptr_t)((a & ~7u) + 4u);
}
API(store_any) void store_any(uint32_t a, uint32_t v) {
    __builtin_memcpy((void *)(uintptr_t)a, &v, 4);
}
API(store_aligned) void store_aligned(uint32_t a, uint32_t v) {
    *(uint32_t *)(uintptr_t)(a & ~3u) = v;
}
API(store_byte) void store_byte(uint32_t a, uint32_t v) { *(uint8_t *)(uintptr_t)a = v; }
/* Alignment must survive a loop-carried pointer, not only a single mask. */
API(load_stride) uint32_t load_stride(uint32_t a, uint32_t count) {
    const uint32_t *p = (const uint32_t *)(uintptr_t)(a & ~3u);
    uint32_t sum = 0;
    for (uint32_t i = 0; i < count; ++i) sum += p[i];
    return sum;
}
API(store_stride) void store_stride(uint32_t a, uint32_t count) {
    uint32_t *p = (uint32_t *)(uintptr_t)(a & ~3u);
    for (uint32_t i = 0; i < count; ++i) p[i] = i * 257u + 3u;
}
API(grow_then_load) uint32_t grow_then_load(uint32_t a, uint32_t pages) {
    (void)__builtin_wasm_memory_grow(0, pages);
    uint32_t v;
    __builtin_memcpy(&v, (void *)(uintptr_t)a, 4);
    return v;
}
