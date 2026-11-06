// SPDX-License-Identifier: Apache-2.0
#include "korvo_camera_buffers.h"
#include <cassert>
#include <cstdio>

int main() {
    uint8_t buffers[2]{};
    auto* first = &buffers[0];
    auto* second = &buffers[1];
    auto* issued = KorvoCameraNextBuffer(first, second, nullptr, nullptr);
    assert(issued == first);
    // The next transaction is requested before completion(first) arrives.
    issued = KorvoCameraNextBuffer(first, second, nullptr, issued);
    assert(issued == second);
    for (unsigned i=0; i<100; ++i) {
        issued = KorvoCameraNextBuffer(first, second, first, issued);
        assert(issued == second);
    }
    assert(KorvoCameraNextBuffer(first, second, second, first) == first);
    assert(KorvoCameraNextBuffer(first, second, nullptr, nullptr) == first);
    puts("Korvo camera DMA: next-before-completed ordering and held-frame isolation passed");
}
