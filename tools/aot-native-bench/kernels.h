#pragma once
#include <stdint.h>
typedef uint32_t (*bench_kernel_t)(uint8_t *, uint32_t, uint32_t);
#define BENCH_KERNELS(X) \
    X(int32_mix) X(int64_mix) X(float32_mac) X(branch_switch) \
    X(call_chain) X(mem32_hot) X(mem32_stream) X(byte_unaligned)
#define DECLARE(name) uint32_t name(uint8_t *, uint32_t, uint32_t);
BENCH_KERNELS(DECLARE)
#undef DECLARE
#define BENCH_BYTES 262144U
#define BENCH_OFFSET 65536U
