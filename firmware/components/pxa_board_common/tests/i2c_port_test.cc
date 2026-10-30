// SPDX-License-Identifier: Apache-2.0
#include "../i2c_port.cc"
#include <cassert>
#include <chrono>
#include <cstdio>
static pxa_peripherals_backend_t backend;
bool pxa_board_add_peripheral_backend(const pxa_peripherals_backend_t* value) { backend = *value; return true; }
static void WaitDriver() {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!entered_driver) { assert(std::chrono::steady_clock::now() < deadline); std::this_thread::yield(); }
}
int main() {
    const uint8_t denied[] = {0x13, 0x14, 0x18, 0x21, 0x30, 0x51};
    assert(pxa_board_i2c_port_initialize(reinterpret_cast<void*>(1), 2, "grove.i2c", denied, sizeof(denied)));
    void *device = nullptr, *other = nullptr;
    for (auto address : denied) assert(backend.open(nullptr, 1, 2, 3, 100000, address, &device) == PXA_STATUS_DENIED);
    assert(backend.open(nullptr, 1, 2, 3, 100000, 0x38, &device) == PXA_STATUS_OK && !add_calls);
    assert(backend.ready(nullptr, device) == PXA_STATUS_WOULD_BLOCK);
    block_add = true;
    std::thread opening([&] { assert(!Process(*Get(device))); });
    WaitDriver();
    assert(backend.ready(nullptr, device) == PXA_STATUS_WOULD_BLOCK);
    assert(backend.open(nullptr, 2, 2, 3, 100000, 0x38, &other) == PXA_STATUS_BUSY);
    release_driver = true; opening.join(); block_add = false;
    assert(backend.ready(nullptr, device) == PXA_STATUS_OK);
    uint8_t tx = 42, rx[256]{}; size_t size = sizeof(rx);
    assert(backend.start(nullptr, device, 1, &tx, 1, 256) == PXA_STATUS_OK);
    tx = 0; assert(!transfer_calls);
    assert(backend.poll(nullptr, device, 1, rx, &size) == PXA_STATUS_WOULD_BLOCK);
    assert(!Process(*Get(device)) && last_tx[0] == 42);
    assert(backend.poll(nullptr, device, 1, rx, &size) == PXA_STATUS_OK && size == 256 && rx[0] == 1 && rx[255] == 0);
    assert(backend.start(nullptr, device, 2, &tx, 1, 2) == PXA_STATUS_OK);
    block_transfer = true; release_driver = entered_driver = false;
    std::thread transfer([&] { assert(Process(*Get(device))); });
    WaitDriver(); backend.cancel(nullptr, device, 2); backend.close(nullptr, device);
    // A pending physical transaction retains its address while Host metadata
    // and independent opens still return immediately.
    assert(backend.open(nullptr, 2, 2, 3, 100000, 0x38, &other) == PXA_STATUS_BUSY);
    assert(backend.open(nullptr, 2, 2, 3, 100000, 0x39, &other) == PXA_STATUS_OK);
    release_driver = true; transfer.join(); block_transfer = false;
    assert(backend.poll(nullptr, device, 2, rx, &size) == PXA_STATUS_CANCELLED);
    fail_remove = true; assert(Process(*Get(device)));
    assert(backend.open(nullptr, 2, 2, 3, 100000, 0x38, &other) == PXA_STATUS_BUSY);
    fail_remove = false; assert(!Process(*Get(device)));
    assert(backend.open(nullptr, 2, 2, 3, 100000, 0x38, &device) == PXA_STATUS_OK);
    fail_add = true; assert(!Process(*Get(device)));
    assert(backend.ready(nullptr, device) == PXA_STATUS_IO_ERROR);
    backend.close(nullptr, device); assert(!Process(*Get(device))); fail_add = false;
    // No queued transaction may start after its resource is closed.
    assert(backend.open(nullptr, 3, 2, 3, 100000, 0x38, &device) == PXA_STATUS_OK);
    assert(!Process(*Get(device)));
    const auto before = transfer_calls;
    assert(backend.start(nullptr, device, 3, &tx, 1, 0) == PXA_STATUS_OK);
    backend.close(nullptr, device); assert(!Process(*Get(device)) && transfer_calls == before);
    puts("External I2C: internal-address denial, deferred opens, exact copied transfers, close during I/O, no late response and cleanup retry passed");
}
