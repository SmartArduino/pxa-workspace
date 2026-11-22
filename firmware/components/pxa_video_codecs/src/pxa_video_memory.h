// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stddef.h>
#include <new>
#include <type_traits>
#include <utility>
void *pxa_video_malloc(size_t);
void pxa_video_free(void *);
template<class T,class... Args> T *pxa_video_new(Args&&... args) {
    void *storage=pxa_video_malloc(sizeof(T));
    return storage ? ::new(storage) T(std::forward<Args>(args)...) : nullptr;
}
template<class T> void pxa_video_delete(T *object) {
    if (object) { object->~T(); pxa_video_free(object); }
}
template<class T> T *pxa_video_array(size_t count) {
    static_assert(std::is_trivially_copyable<T>::value,"Codec thread records must remain trivial");
    if (count>size_t(-1)/sizeof(T)) return nullptr;
    void *storage=pxa_video_malloc(count*sizeof(T));
    return storage ? ::new(storage) T[count] : nullptr;
}
