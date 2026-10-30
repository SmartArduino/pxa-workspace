// SPDX-License-Identifier: Apache-2.0
#include "../rgb_port.cc"
#include <atomic>
#include <cassert>
#include <thread>
#include <cstdio>
static pxa_peripherals_backend_t backend;
bool pxa_board_add_peripheral_backend(const pxa_peripherals_backend_t* value) { backend = *value; return true; }
static std::atomic<bool> block{false}, entered{false}, release_io{false};
static unsigned applied, calls;
static bool fail_black;
static bool Apply(void*, unsigned r, unsigned g, unsigned b) {
    if (block) { entered = true; while (!release_io) std::this_thread::yield(); }
    applied = (r << 16) | (g << 8) | b; ++calls;
    return !(fail_black && !applied);
}
int main(int argc, char**) {
    if (argc > 1) {
        assert(pxa_board_binary_indicator_initialize(nullptr, [](void*, bool on) { applied = on; ++calls; return true; }));
        size_t count = 0; pxa_peripheral_port_t port{};
        assert(backend.list(nullptr, &port, 1, &count) == PXA_STATUS_OK && count == 1);
        assert(port.kind == PXA_PERIPHERALS_GPIO && !std::strcmp(port.name, "indicator.status"));
        void* device = nullptr;
        assert(backend.open(nullptr, 1, 1, PXA_PERIPHERALS_MODE_RGB, 1, 0, &device) == PXA_STATUS_INVALID_ARGUMENT);
        assert(backend.open(nullptr, 1, 1, PXA_PERIPHERALS_MODE_OUTPUT, 2, 0, &device) == PXA_STATUS_INVALID_ARGUMENT);
        assert(backend.open(nullptr, 1, 1, PXA_PERIPHERALS_MODE_OUTPUT, 1, 0, &device) == PXA_STATUS_OK && !calls);
        assert(!ApplyPending() && applied == 1);
        uint8_t value = 2;
        assert(backend.io(nullptr, device, PXA_IO_WRITE, &value, 1) == PXA_STATUS_INVALID_ARGUMENT);
        value = 0; assert(backend.io(nullptr, device, PXA_IO_WRITE, &value, 1) == 1);
        assert(!ApplyPending() && !applied);
        backend.close(nullptr, device); assert(!ApplyPending() && !applied);
        puts("Binary indicator: logical on/off, actual GPIO kind, bounded asynchronous update and close cleanup passed");
        return 0;
    }
    assert(!pxa_board_rgb_port_initialize(nullptr, nullptr));
    assert(pxa_board_rgb_port_initialize(nullptr, Apply));
    assert(!pxa_board_rgb_port_initialize(nullptr, Apply));
    size_t count = 0; pxa_peripheral_port_t port{};
    assert(backend.list(nullptr, &port, 0, &count) == PXA_STATUS_LIMIT_EXCEEDED);
    assert(backend.list(nullptr, &port, 1, &count) == PXA_STATUS_OK && count == 1);
    assert(port.kind == PXA_PERIPHERALS_RGB && !std::strcmp(port.name, "indicator.rgb"));
    void* device = nullptr; void* next = nullptr;
    assert(backend.open(nullptr, 1, 1, PXA_PERIPHERALS_MODE_RGB, 0x1000000, 0, &device) == PXA_STATUS_INVALID_ARGUMENT);
    assert(backend.open(nullptr, 1, 1, PXA_PERIPHERALS_MODE_RGB, 0x123456, 0, &device) == PXA_STATUS_OK);
    assert(!calls); // No driver I/O on Host open.
    assert(!ApplyPending() && applied == 0x123456 && calls == 1);
    uint8_t color[] = {0xAB, 0xCD, 0xEF};
    assert(backend.io(nullptr, device, PXA_IO_READ, color, 3) == PXA_STATUS_UNSUPPORTED);
    assert(backend.io(nullptr, device, PXA_IO_WRITE, color, 2) == PXA_STATUS_INVALID_ARGUMENT);
    assert(backend.io(nullptr, device, PXA_IO_WRITE, color, 3) == 3);
    block = true;
    std::thread worker([] { assert(ApplyPending()); });
    while (!entered) std::this_thread::yield();
    backend.close(nullptr, device); // Driver is blocked, cleanup still returns.
    assert(backend.open(nullptr, 2, 1, PXA_PERIPHERALS_MODE_RGB, 0, 0, &next) == PXA_STATUS_BUSY);
    assert(backend.io(nullptr, device, PXA_IO_WRITE, color, 3) == PXA_STATUS_UNAVAILABLE);
    release_io = true; worker.join(); block = false;
    fail_black = true;
    assert(ApplyPending()); // Failed black write retains reservation.
    assert(backend.open(nullptr, 2, 1, PXA_PERIPHERALS_MODE_RGB, 0, 0, &next) == PXA_STATUS_BUSY);
    fail_black = false;
    assert(!ApplyPending() && applied == 0);
    assert(backend.open(nullptr, 2, 1, PXA_PERIPHERALS_MODE_RGB, 0, 0, &next) == PXA_STATUS_OK);
    color[0] = 1;
    assert(backend.io(nullptr, next, PXA_IO_WRITE, color, 3) == 3);
    color[0] = 2;
    assert(backend.io(nullptr, next, PXA_IO_WRITE, color, 3) == 3);
    assert(!ApplyPending() && applied == 0x02CDEF); // Latest accepted color wins.
    backend.close(nullptr, next); assert(!ApplyPending() && applied == 0);
    puts("RGB port: exact scope, nonblocking driver handoff, coalescing, close during I/O, cleanup retry and ownership passed");
}
