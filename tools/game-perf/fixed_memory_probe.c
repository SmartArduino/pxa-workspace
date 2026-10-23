#include <stdint.h>
#define API(n) __attribute__((export_name(#n), noinline))
__attribute__((import_module("env"), import_name("mutate")))
extern uint32_t mutate(uint32_t address, uint32_t value, uint32_t pages);
static uint32_t growth;
static inline uint32_t load(uint32_t address) {
    uint32_t value;
    __builtin_memcpy(&value, (void *)(uintptr_t)address, 4);
    return value;
}
API(pages) uint32_t pages(void) { return __builtin_wasm_memory_size(0); }
API(growth_result) uint32_t growth_result(void) { return growth; }
API(read_word) uint32_t read_word(uint32_t address) {
    return load(address);
}
API(write_word) void write_word(uint32_t address, uint32_t value) {
    __builtin_memcpy((void *)(uintptr_t)address, &value, 4);
}
API(call_then_read) uint32_t call_then_read(uint32_t address, uint32_t value,
                                          uint32_t extra_pages) {
    uint32_t before = load(address);
    growth = mutate(address, value, extra_pages);
    return before ^ load(address);
}
API(call_then_read_end) uint32_t call_then_read_end(uint32_t address,
                                                 uint32_t extra_pages) {
    growth = mutate(32768, 123, extra_pages);
    return load(address);
}
API(grow_then_read) uint32_t grow_then_read(uint32_t address, uint32_t extra_pages) {
    growth = __builtin_wasm_memory_grow(0, extra_pages);
    return load(address);
}
API(loop_calls) uint32_t loop_calls(uint32_t address, uint32_t count) {
    uint32_t sum = 0;
    for (uint32_t i = 0; i < count; ++i) {
        mutate(address, i, 0);
        sum += load(address);
    }
    return sum;
}
/* Every input resolves within the module's minimum allocation. */
API(bounded_read) uint32_t bounded_read(uint32_t index) {
    return load(32768 + (index & 255u) * 4u);
}
API(bounded_write) void bounded_write(uint32_t index, uint32_t value) {
    uint32_t address = 32768 + (index & 255u) * 4u;
    __builtin_memcpy((void *)(uintptr_t)address, &value, 4);
}
