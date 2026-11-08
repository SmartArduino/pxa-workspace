#pragma once
#include <cstddef>
constexpr int MALLOC_CAP_INTERNAL = 1, MALLOC_CAP_DMA = 2, MALLOC_CAP_8BIT = 4;
void* heap_caps_aligned_alloc(size_t, size_t, unsigned);
void heap_caps_free(void*);
