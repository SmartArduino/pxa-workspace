// SPDX-License-Identifier: Apache-2.0
#include "espp4_a_camera_buffers.h"
#include <cassert>
#include <cstdio>
int main() {
    assert(espp4_a::CameraFrameFresh(100, 80100));
    assert(!espp4_a::CameraFrameFresh(100, 80101));
    assert(espp4_a::CameraFrameFresh(0xfffff000u, 0x1000u));
    assert(!espp4_a::CameraFrameFresh(0xfffff000u, 0x20000u));
    int first, second;
    // Explore worker release before next-buffer selection, between callbacks,
    // after completion, and while DMA repeatedly overwrites its scratch frame.
    for (unsigned release = 0; release < 4; ++release) {
        void* held = &first;
        void* current = &second;
        for (unsigned frame = 0; frame < 12; ++frame) {
            if (release == 0 || (release == 3 && frame == 4)) held = nullptr;
            void* next = espp4_a::CameraNextBuffer(&first, &second, held, current);
            assert(next != held);
            if (release == 1) held = nullptr;
            if (espp4_a::CameraCanPublish(current, next, held, 64, 64)) held = current;
            assert(held != next); // the published frame never belongs to active DMA
            if (release == 2) held = nullptr;
            current = next;
        }
    }
    assert(!espp4_a::CameraCanPublish(&first, &second, nullptr, 63, 64));
    assert(!espp4_a::CameraCanPublish(&first, &first, nullptr, 64, 64));
    assert(!espp4_a::CameraCanPublish(&first, &second, &first, 64, 64));
    std::puts("camera DMA ownership interleavings passed");
}
