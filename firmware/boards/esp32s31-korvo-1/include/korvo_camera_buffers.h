// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>

// IDF queues the next DVP transfer before reporting the completed one. Until
// completion is published, exclude the last issued buffer from reuse.
inline uint8_t* KorvoCameraNextBuffer(uint8_t* first, uint8_t* second,
    const uint8_t* completed, const uint8_t* last_issued) {
    const auto* excluded = completed ? completed : last_issued;
    return excluded == first ? second : first;
}
