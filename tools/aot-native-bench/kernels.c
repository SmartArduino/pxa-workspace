/* Identical translation unit for native and wasm. No timing or host APIs. */
#include "kernels.h"
#ifdef __wasm__
#define KERNEL(name) __attribute__((export_name(#name), noinline)) uint32_t name
#else
#define KERNEL(name) __attribute__((noinline)) uint32_t name
#endif
static inline uint32_t rotate(uint32_t x, unsigned n) {
    return (x << n) | (x >> (32U - n));
}
KERNEL(int32_mix)(uint8_t *p, uint32_t n, uint32_t x) {
    (void)p;
    for (uint32_t i = 0; i < n; ++i) {
        x ^= x >> 13; x *= 0x85ebca6bU;
        x = rotate(x, 7) + i; x ^= x >> 16;
    }
    return x;
}
KERNEL(int64_mix)(uint8_t *p, uint32_t n, uint32_t seed) {
    (void)p;
    uint64_t x = ((uint64_t)seed << 32) | (seed ^ 0xdeadbeefU);
    for (uint32_t i = 0; i < n; ++i) {
        x ^= x >> 27; x *= UINT64_C(0x2545f4914f6cdd1d);
        x = ((x << 17) | (x >> 47)) + i;
    }
    return (uint32_t)x ^ (uint32_t)(x >> 32);
}
KERNEL(float32_mac)(uint8_t *p, uint32_t n, uint32_t seed) {
    (void)p;
    float x = (float)(seed & 1023U) / 1024.0f;
    float y = 0.125f;
    for (uint32_t i = 0; i < n; ++i) {
        x = x * 0.99991f + y * 0.00007f;
        y = y * 0.99997f + (float)(i & 15U) * 0.00001f;
    }
    union { float f; uint32_t u; } result = {.f = x + y};
    return result.u;
}
KERNEL(branch_switch)(uint8_t *p, uint32_t n, uint32_t x) {
    (void)p;
    for (uint32_t i = 0; i < n; ++i) {
        switch (x & 7U) {
        case 0: x = rotate(x, 3) + 0x12345U; break;
        case 1: x = (x ^ 0xdeadbeefU) * 17U; break;
        case 2: x = rotate(x, 13) ^ i; break;
        case 3: x = (x >> 7) + 0x9e3779b9U; break;
        case 4: x = (x << 9) ^ 0xabcdefU; break;
        case 5: x = x * 33U + i; break;
        case 6: x = rotate(x, 17) - 0x4567U; break;
        default: x = (x ^ (x >> 11)) + 0x89abU; break;
        }
        x += i | 1U;
    }
    return x;
}
static __attribute__((noinline)) uint32_t helper(uint32_t x, uint32_t y) {
    return rotate(x ^ y, 11) * 0x9e3779b9U;
}
/* Volatile indirection preserves calls in both C and Wasm optimization paths. */
static uint32_t (*volatile helper_ptr)(uint32_t, uint32_t) = helper;
KERNEL(call_chain)(uint8_t *p, uint32_t n, uint32_t x) {
    (void)p;
    for (uint32_t i = 0; i < n; ++i)
        x = helper_ptr(helper_ptr(x, i), x >> 3);
    return x;
}
#define WORD_KERNEL(name, mask) \
KERNEL(name)(uint8_t *p, uint32_t n, uint32_t x) { \
    uint32_t *words = (uint32_t *)p; \
    for (uint32_t i = 0; i < n; ++i) { \
        uint32_t at = (i * 17U) & (mask); \
        x = rotate(x + words[at], 5); words[at] = x ^ i; \
    } \
    return x; \
}
WORD_KERNEL(mem32_hot, 1023U)
WORD_KERNEL(mem32_stream, 65535U)
KERNEL(byte_unaligned)(uint8_t *p, uint32_t n, uint32_t x) {
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t at = (i * 13U) & 4095U;
        uint32_t v = (uint32_t)p[at] | (uint32_t)p[at+1] << 8 |
                     (uint32_t)p[at+2] << 16 | (uint32_t)p[at+3] << 24;
        x = rotate(x ^ v, 7); p[at + 1] = (uint8_t)x;
    }
    return x;
}
