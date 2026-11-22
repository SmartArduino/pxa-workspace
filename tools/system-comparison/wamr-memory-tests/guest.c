/* SPDX-License-Identifier: MIT */
typedef unsigned int u32;
typedef unsigned char u8;
static volatile u32 static_words[256];
__attribute__((import_module("env"), import_name("host_grow")))
extern int host_grow(unsigned pages);

int static_loop(void)
{
    u32 total = 0;
    for (u32 i = 0; i < 256; ++i) static_words[i] = i;
    for (u32 pass = 0; pass < 32; ++pass)
        for (u32 i = 0; i < 256; ++i) static_words[i] += 1;
    for (u32 i = 0; i < 256; ++i) total += static_words[i];
    return total != 40832;
}
u32 static_dynamic_index(u32 index) { return static_words[index]; }

static u32 size(void) { return __builtin_wasm_memory_size(0); }
__attribute__((noinline)) static int grow_one(unsigned ignored)
{ (void)ignored; return __builtin_wasm_memory_grow(0, 1) != (u32)-1; }

int grow_direct(void)
{
    for (u32 i = 0; i < 3; ++i) {
        u32 old = size();
        volatile u32 *last = (volatile u32 *)(old * 65536u - 4);
        *last = 0x12340000u + i;
        if (__builtin_wasm_memory_grow(0, 1) != old) return 1;
        if (*last != 0x12340000u + i) return 2;
        volatile u32 *fresh = (volatile u32 *)(old * 65536u);
        if (*fresh != 0) return 3;
        *fresh = 0x87650000u + i;
        if (*fresh != 0x87650000u + i) return 4;
    }
    return 0;
}

int grow_indirect(void)
{
    int (*volatile callback)(unsigned) = grow_one;
    for (u32 i = 0; i < 3; ++i) {
        u32 old = size();
        if (!callback(i) || size() != old + 1) return 1;
        volatile u32 *fresh = (volatile u32 *)(old * 65536u);
        *fresh = 0xabcdef00u + i;
        if (*fresh != 0xabcdef00u + i) return 2;
    }
    return 0;
}

int grow_native(void)
{
    for (u32 i = 0; i < 3; ++i) {
        u32 old = size();
        volatile u32 *last = (volatile u32 *)(old * 65536u - 4);
        *last = 0x13570000u + i;
        if (!host_grow(1) || size() != old + 1) return 1;
        if (*last != 0x13570000u + i) return 2;
        volatile u32 *fresh = (volatile u32 *)(old * 65536u);
        *fresh = 0x24680000u + i;
        if (*fresh != 0x24680000u + i) return 3;
    }
    return 0;
}

int grow_failure(void)
{
    while (size() < 8) if (!grow_one(0)) return 1;
    volatile u32 *last = (volatile u32 *)(size() * 65536u - 4);
    *last = 0x24681357;
    if (__builtin_wasm_memory_grow(0, 1) != (u32)-1) return 2;
    return size() != 8 || *last != 0x24681357;
}

u32 alias_words(u32 p, u32 q, u32 value)
{
    *(u32 *)p = value;
    *(float *)q = 0.0f;
    return *(u32 *)p;
}

u32 alias_bytes(u32 p, u32 q, u32 value)
{
    *(u32 *)p = value;
    *(u8 *)q = 0;
    return *(u32 *)p;
}

u32 trap_read(void) { return *(volatile u32 *)(size() * 65536u); }
void trap_write(void) { *(volatile u32 *)(size() * 65536u) = 17; }
void trap_crossing(void) { *(volatile u32 *)(size() * 65536u - 2) = 17; }

__attribute__((noinline)) u32 offset_load(u32 base)
{ return ((volatile u32 *)base)[16]; }
__attribute__((noinline)) void offset_store(u32 base, u32 value)
{ ((volatile u32 *)base)[16] = value; }
int offset_valid(void)
{
    u32 base = size() * 65536u - 68;
    offset_store(base, 0x24681357);
    return offset_load(base) != 0x24681357;
}
u32 trap_offset_end(void) { return offset_load(size() * 65536u - 64); }
u32 trap_offset_wrap(void) { return offset_load(0xfffffff0u); }
void trap_offset_store_wrap(void) { offset_store(0xfffffff0u, 19); }

int grow_offset_preserve(u32 base)
{
    ((volatile u32 *)base)[16] = 0x13572468;
    if (!host_grow(1)) return 1;
    return ((volatile u32 *)base)[16] != 0x13572468;
}
int grow_offset_fresh(u32 base)
{
    if (base != size() * 65536u - 64) return 1;
    if (!host_grow(1)) return 2;
    ((volatile u32 *)base)[16] = 0x12345678;
    return ((volatile u32 *)base)[16] != 0x12345678;
}
