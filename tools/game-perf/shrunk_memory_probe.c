#include <stdint.h>
#define API(n) __attribute__((export_name(#n), noinline))
/* No memory.size or memory.grow: the pinned loader may shrink this module to
 * its exported heap base. Bounds metadata must use that final descriptor. */
API(read_word) uint32_t read_word(uint32_t address) {
    uint32_t value;
    __builtin_memcpy(&value, (void *)(uintptr_t)address, 4);
    return value;
}
API(write_word) void write_word(uint32_t address, uint32_t value) {
    __builtin_memcpy((void *)(uintptr_t)address, &value, 4);
}
API(bounded_read) uint32_t bounded_read(uint32_t index) {
    uint32_t value;
    __builtin_memcpy(&value, (void *)(uintptr_t)(32768 + (index & 255u) * 4u), 4);
    return value;
}
API(bounded_write) void bounded_write(uint32_t index, uint32_t value) {
    __builtin_memcpy((void *)(uintptr_t)(32768 + (index & 255u) * 4u), &value, 4);
}
