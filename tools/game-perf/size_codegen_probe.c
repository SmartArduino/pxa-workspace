#include <stdint.h>
#define API(n) __attribute__((export_name(#n), noinline))

/* RV32 must lower variable i64 shifts without additional Host symbols. */
API(left) uint64_t left(uint64_t value, uint32_t count) {
    return value << (count & 63u);
}
API(right) uint64_t right(uint64_t value, uint32_t count) {
    return value >> (count & 63u);
}
API(signed_right) int64_t signed_right(int64_t value, uint32_t count) {
    return value >> (count & 63u);
}
