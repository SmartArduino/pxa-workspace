// SPDX-License-Identifier: Apache-2.0
#include "../vision_port.cc"
#include <atomic>
#include <cassert>
#include <cstdlib>
#include <thread>
#include <vector>
#include <cstdio>
static pxa_peripherals_backend_t backend;
static unsigned allocations, preparations, captures, finishes;
static bool deny_memory, oversized;
static std::atomic<bool> block{false}, entered{false}, released{false};
bool pxa_board_add_peripheral_backend(const pxa_peripherals_backend_t* b) { backend = *b; return true; }
extern "C" const pxa_memory_allocator_t* pxa_esp_device_resource_allocator(unsigned c, unsigned k) {
    assert(c == PXA_MEMORY_EXTERNAL && k == PXA_MEMORY_FRAME); return nullptr;
}
extern "C" void* pxa_memory_allocate(const pxa_memory_allocator_t*, size_t n) {
    if (deny_memory) return nullptr;
    ++allocations; return std::malloc(n);
}
extern "C" void pxa_memory_release(void* p) { if (p) { --allocations; std::free(p); } }
static pxa_status_t Prepare(void*, uint16_t format) { assert(format == PXA_VISION_JPEG); ++preparations; return PXA_STATUS_OK; }
static pxa_status_t Capture(void*, uint8_t* p, size_t cap, size_t* n) {
    ++captures;
    if (block) { entered = true; while (!released) std::this_thread::yield(); }
    *n = oversized ? cap + 1 : 600;
    if (!oversized) for (size_t i = 0; i < *n; ++i) p[i] = uint8_t(i);
    return PXA_STATUS_OK;
}
static void Finish(void*) { ++finishes; }
int main() {
    const PxaBoardVisionDriver driver{nullptr, true, false, Prepare, Capture, Finish};
    assert(pxa_board_vision_port_initialize(&driver));
    assert(!pxa_board_vision_port_initialize(&driver));
    size_t count; pxa_peripheral_port_t port{};
    assert(backend.list(nullptr, &port, 1, &count) == PXA_STATUS_OK && count == 1);
    assert(port.kind == PXA_PERIPHERALS_VISION && port.id == 3 && port.flags == PXA_PERIPHERALS_READ && !strcmp(port.name, "camera.jpeg"));
    void* handle = nullptr; void* next = nullptr;
    assert(backend.open(nullptr, 1, 4, 7, 0, 0, &handle) == PXA_STATUS_UNSUPPORTED);
    assert(backend.open(nullptr, 1, 3, 7, 1, 0, &handle) == PXA_STATUS_INVALID_ARGUMENT);
    assert(backend.open(nullptr, 1, 3, 7, 0, 0, &handle) == PXA_STATUS_OK);
    assert(!preparations && !allocations && !captures);
    assert(backend.ready(nullptr, handle) == PXA_STATUS_WOULD_BLOCK);
    assert(Pump() && allocations == 1 && preparations == 1);
    assert(backend.ready(nullptr, handle) == PXA_STATUS_OK);
    uint8_t bytes[256];
    assert(backend.io(nullptr, handle, PXA_IO_READ, bytes, 100) == PXA_STATUS_WOULD_BLOCK);
    assert(!Pump() && captures == 1);
    assert(!Pump() && captures == 1); // Never overwrite an unconsumed frame.
    assert(backend.io(nullptr, handle, PXA_IO_WRITE, bytes, 1) == PXA_STATUS_UNSUPPORTED);
    std::vector<uint8_t> frame;
    while (frame.size() < 624) {
        const auto n = backend.io(nullptr, handle, PXA_IO_READ, bytes, 17);
        assert(n > 0); frame.insert(frame.end(), bytes, bytes + n);
    }
    pxa_vision_frame_info_t info{}; assert(pxa_vision_decode(frame.data(), &info));
    assert(info.size == 600 && info.sequence == 1 && info.monotonic_us == 123456789);
    for (size_t i = 0; i < 600; ++i) assert(frame[24 + i] == uint8_t(i));
    block = true;
    std::thread worker([] { assert(Pump()); });
    while (!entered) std::this_thread::yield();
    backend.close(nullptr, handle); // In-flight capture does not block close.
    assert(backend.open(nullptr, 2, 3, 7, 0, 0, &next) == PXA_STATUS_BUSY);
    assert(backend.io(nullptr, handle, PXA_IO_READ, bytes, 1) == PXA_STATUS_UNAVAILABLE);
    released = true; worker.join(); block = false;
    assert(!Pump() && !allocations && finishes == 1);
    assert(backend.open(nullptr, 2, 3, 7, 0, 0, &next) == PXA_STATUS_OK);
    deny_memory = true; assert(!Pump());
    assert(backend.ready(nullptr, next) == PXA_STATUS_RESOURCE_LIMIT && preparations == 1);
    backend.close(nullptr, next); assert(!Pump() && !allocations);
    deny_memory = false;
    assert(backend.open(nullptr, 3, 3, 7, 0, 0, &next) == PXA_STATUS_OK);
    assert(Pump()); oversized = true; assert(!Pump());
    assert(backend.io(nullptr, next, PXA_IO_READ, bytes, 1) == PXA_STATUS_PROTOCOL_ERROR);
    backend.close(nullptr, next); assert(!Pump() && !allocations);
    puts("Vision native: no pre-permission capture, bounded memory, partial immutable frames, capture/close race, rollback and oversize rejection passed");
}
