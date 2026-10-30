// SPDX-License-Identifier: Apache-2.0
#include "../peripheral_backends.cc"
#include <cassert>
#include <cstdio>
static pxa_peripherals_backend_t mux;
extern "C" bool pxa_esp_peripherals_set_backend(const pxa_peripherals_backend_t* value) { mux = *value; return true; }
struct Fake { uint16_t id; const char* scope; unsigned opens = 0, closes = 0, starts = 0, polls = 0, cancels = 0; bool fail = false; } first{1,"indicator.rgb"}, second{2,"grove.i2c"};
static pxa_status_t FakeList(void* c, pxa_peripheral_port_t* ports, size_t capacity, size_t* n) {
    auto& f = *static_cast<Fake*>(c); *n = 1;
    if (!capacity) return PXA_STATUS_LIMIT_EXCEEDED;
    *ports = {}; ports->id = f.id; ports->kind = 2; std::strcpy(ports->name, f.scope); return PXA_STATUS_OK;
}
static pxa_status_t FakeOpen(void* c, uint64_t, uint16_t id, uint16_t, uint32_t, uint16_t, void** out) {
    auto& f = *static_cast<Fake*>(c); assert(id == f.id); ++f.opens; *out = c;
    return f.fail ? PXA_STATUS_IO_ERROR : PXA_STATUS_OK;
}
static pxa_status_t FakeReady(void*, void*) { return PXA_STATUS_WOULD_BLOCK; }
static pxa_status_t FakeModule(void*, uint16_t, pxa_peripheral_module_t*) { return PXA_STATUS_NOT_FOUND; }
static int32_t FakeIo(void* c, void* d, uint32_t, uint8_t*, size_t n) { assert(c == d); return int32_t(n); }
static pxa_status_t FakeStart(void* c, void* d, uint64_t cookie, const uint8_t*, uint16_t, uint16_t) { assert(c == d && cookie == 42); ++static_cast<Fake*>(c)->starts; return PXA_STATUS_OK; }
static pxa_status_t FakePoll(void* c, void* d, uint64_t cookie, uint8_t*, size_t*) { assert(c == d && cookie == 42); ++static_cast<Fake*>(c)->polls; return PXA_STATUS_WOULD_BLOCK; }
static void FakeCancel(void* c, void* d, uint64_t cookie) { assert(c == d && cookie == 42); ++static_cast<Fake*>(c)->cancels; }
static void FakeClose(void* c, void* d) { assert(c == d); ++static_cast<Fake*>(c)->closes; }
int main() {
    pxa_peripherals_backend_t a{sizeof(a), &first, FakeList, FakeModule, FakeOpen, FakeIo, FakeStart, FakePoll, FakeCancel, FakeClose, nullptr};
    auto b = a; b.context = &second; b.ready = FakeReady;
    assert(pxa_board_add_peripheral_backend(&a));
    assert(!pxa_board_add_peripheral_backend(&a)); // Duplicate IDs/scopes rejected before publication.
    assert(pxa_board_add_peripheral_backend(&b));
    pxa_peripheral_port_t listed[2]; size_t n = 0;
    assert(mux.list(nullptr, listed, 2, &n) == PXA_STATUS_OK && n == 2 && listed[1].id == 2);
    void *one = nullptr, *two = nullptr;
    assert(mux.open(nullptr, 1, 1, 6, 0, 0, &one) == PXA_STATUS_OK);
    assert(mux.open(nullptr, 1, 2, 3, 100000, 0x38, &two) == PXA_STATUS_OK);
    assert(one != &first && two != &second && one != two);
    assert(mux.ready(nullptr, one) == PXA_STATUS_OK && mux.ready(nullptr, two) == PXA_STATUS_WOULD_BLOCK);
    uint8_t byte = 0; assert(mux.io(nullptr, one, 2, &byte, 1) == 1);
    assert(mux.start(nullptr, two, 42, &byte, 1, 1) == PXA_STATUS_OK);
    assert(mux.poll(nullptr, two, 42, &byte, &n) == PXA_STATUS_WOULD_BLOCK);
    mux.cancel(nullptr, two, 42); mux.close(nullptr, one); mux.close(nullptr, two);
    assert(first.closes == 1 && second.closes == 1 && second.starts == 1 && second.polls == 1 && second.cancels == 1);
    mux.close(nullptr, two); assert(second.closes == 1);
    first.fail = true;
    assert(mux.open(nullptr, 1, 1, 6, 0, 0, &one) == PXA_STATUS_IO_ERROR && one);
    mux.close(nullptr, one); assert(first.closes == 2); first.fail = false;
    void* handles[16];
    for (auto& handle : handles) assert(mux.open(nullptr, 1, 1, 6, 0, 0, &handle) == PXA_STATUS_OK);
    assert(mux.open(nullptr, 1, 1, 6, 0, 0, &one) == PXA_STATUS_RESOURCE_LIMIT && !one);
    for (auto handle : handles) mux.close(nullptr, handle);
    puts("Peripheral providers: unique discovery, bounded wrapper ownership, async forwarding and failed-open cleanup passed");
}
