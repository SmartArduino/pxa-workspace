/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
uint32_t bench_kernel(unsigned kind);
void bench_paint(uint16_t *pixels,unsigned width,unsigned height,unsigned pitch,unsigned mode,unsigned phase);
uint32_t bench_checksum(const uint16_t *pixels,unsigned width,unsigned height,unsigned pitch);
#ifdef __cplusplus
}
#endif
