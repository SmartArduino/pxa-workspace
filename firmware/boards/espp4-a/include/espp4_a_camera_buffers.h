// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstddef>
#include <cstdint>
namespace espp4_a {
inline bool CameraFrameFresh(uint32_t ready_us, uint32_t now_us) {
    // After storage backpressure, wait for a current frame rather than showing
    // the mailbox image that may have been held for several seconds.
    return uint32_t(now_us - ready_us) <= 80000;
}
// CSI requests the next DMA target before reporting the previous completion.
// A worker-held frame is immutable; the other buffer may be reused by DMA.
inline __attribute__((always_inline)) void* CameraNextBuffer(void* first, void* second,
    const void* held, const void* last) {
    return held == first ? second : held == second ? first : last == first ? second : first;
}
inline __attribute__((always_inline)) bool CameraCanPublish(const void* finished,
    const void* next, const void* held, size_t received, size_t required) {
    // Reusing the current buffer is safe only while dropping its completion.
    // The worker can release its old frame between the two ISR callbacks.
    return finished && finished != next && !held && received >= required;
}
}
