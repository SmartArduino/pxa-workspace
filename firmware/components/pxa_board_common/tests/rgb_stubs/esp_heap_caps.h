// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cassert>
#include <cstdlib>
constexpr unsigned MALLOC_CAP_SPIRAM=1, MALLOC_CAP_8BIT=2;
inline bool pxa_test_heap_fail=false;
inline unsigned pxa_test_heap_live=0;
inline void* heap_caps_malloc(size_t bytes, unsigned caps) {
    assert(caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    if(pxa_test_heap_fail)return nullptr;
    void* result=std::malloc(bytes);
    if(result)++pxa_test_heap_live;
    return result;
}
inline void heap_caps_free(void* pointer) {
    if(!pointer)return;
    assert(pxa_test_heap_live);--pxa_test_heap_live;std::free(pointer);
}
