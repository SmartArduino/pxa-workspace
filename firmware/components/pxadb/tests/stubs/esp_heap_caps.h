#pragma once
#include <cstddef>
#include <cstdint>
constexpr uint32_t MALLOC_CAP_8BIT = 0x4;
constexpr uint32_t MALLOC_CAP_SPIRAM = 0x400;
constexpr uint32_t MALLOC_CAP_INTERNAL = 0x800;
extern "C" {
void* heap_caps_malloc(size_t, uint32_t);
void* heap_caps_calloc(size_t, size_t, uint32_t);
void* heap_caps_aligned_calloc(size_t, size_t, size_t, uint32_t);
void heap_caps_free(void*);
}
