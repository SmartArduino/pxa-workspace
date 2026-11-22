/* SPDX-License-Identifier: MIT */
typedef unsigned int u32;
int zero_then_grow(void)
{
    if (__builtin_wasm_memory_size(0) != 0) return 1;
    if (__builtin_wasm_memory_grow(0, 1) != 0) return 2;
    volatile u32 *value = (volatile u32 *)4160;
    *value = 0x13572468;
    return *value != 0x13572468;
}
u32 zero_trap(void) { return *(volatile u32 *)4160; }
