// SPDX-License-Identifier: Apache-2.0
#include <pxa_board_common/peripheral_backends.h>
#include <cstring>

namespace {
pxa_peripherals_backend_t providers[4];
size_t count;
struct Device { const pxa_peripherals_backend_t* provider = nullptr; void* native = nullptr; } devices[16];
pxa_peripheral_port_t ports[PXA_PERIPHERALS_MAX_PORTS];
Device* Get(void* native) {
    for (auto& device : devices) if (&device == native && device.provider) return &device;
    return nullptr;
}
pxa_status_t List(void*, pxa_peripheral_port_t* output, size_t capacity, size_t* total) {
    *total = 0;
    for (size_t i = 0; i < count; ++i) {
        size_t added = 0;
        const auto status = providers[i].list(providers[i].context, output + *total, capacity - *total, &added);
        if (status != PXA_STATUS_OK) return status;
        if (added > capacity - *total) return PXA_STATUS_PROTOCOL_ERROR;
        *total += added;
    }
    return PXA_STATUS_OK;
}
const pxa_peripherals_backend_t* Find(uint16_t id) {
    for (size_t i = 0; i < count; ++i) {
        size_t size = 0;
        if (providers[i].list(providers[i].context, ports, PXA_PERIPHERALS_MAX_PORTS, &size) != PXA_STATUS_OK || size > PXA_PERIPHERALS_MAX_PORTS)
            continue;
        for (size_t p = 0; p < size; ++p) if (ports[p].id == id) return &providers[i];
    }
    return nullptr;
}
pxa_status_t Module(void*, uint16_t port, pxa_peripheral_module_t* output) {
    const auto* p = Find(port);
    return p ? p->module(p->context, port, output) : PXA_STATUS_NOT_FOUND;
}
pxa_status_t Open(void*, uint64_t owner, uint16_t port, uint16_t mode, uint32_t parameter, uint16_t address, void** output) {
    *output = nullptr;
    const auto* p = Find(port);
    if (!p) return PXA_STATUS_NOT_FOUND;
    for (auto& device : devices) if (!device.provider) {
        device.provider = p; device.native = nullptr; *output = &device;
        return p->open(p->context, owner, port, mode, parameter, address, &device.native);
    }
    return PXA_STATUS_RESOURCE_LIMIT;
}
pxa_status_t Ready(void*, void* native) {
    auto* d = Get(native);
    if (!d || !d->native) return PXA_STATUS_PROTOCOL_ERROR;
    return d->provider->ready ? d->provider->ready(d->provider->context, d->native) : PXA_STATUS_OK;
}
int32_t Io(void*, void* native, uint32_t op, uint8_t* bytes, size_t size) {
    auto* d = Get(native);
    return d && d->native ? d->provider->io(d->provider->context, d->native, op, bytes, size) : PXA_STATUS_UNAVAILABLE;
}
pxa_status_t Start(void*, void* native, uint64_t cookie, const uint8_t* tx, uint16_t n, uint16_t rx) {
    auto* d = Get(native);
    return d && d->native ? d->provider->start(d->provider->context, d->native, cookie, tx, n, rx) : PXA_STATUS_UNAVAILABLE;
}
pxa_status_t Poll(void*, void* native, uint64_t cookie, uint8_t* rx, size_t* n) {
    auto* d = Get(native);
    return d && d->native ? d->provider->poll(d->provider->context, d->native, cookie, rx, n) : PXA_STATUS_UNAVAILABLE;
}
void Cancel(void*, void* native, uint64_t cookie) {
    if (auto* d = Get(native); d && d->native) d->provider->cancel(d->provider->context, d->native, cookie);
}
void Close(void*, void* native) {
    if (auto* d = Get(native)) {
        if (d->native) d->provider->close(d->provider->context, d->native);
        *d = {};
    }
}
}
bool pxa_board_add_peripheral_backend(const pxa_peripherals_backend_t* p) {
    if (count == 4 || !p || p->struct_size < PXA_PERIPHERALS_BACKEND_BASE_SIZE ||
        !p->list || !p->module || !p->open || !p->io || !p->start || !p->poll || !p->cancel || !p->close) return false;
    pxa_peripheral_port_t added[PXA_PERIPHERALS_MAX_PORTS]; size_t added_count = 0;
    if (p->list(p->context, added, PXA_PERIPHERALS_MAX_PORTS, &added_count) != PXA_STATUS_OK ||
        !added_count || added_count > PXA_PERIPHERALS_MAX_PORTS) return false;
    size_t total = added_count;
    for (size_t i = 0; i < count; ++i) {
        size_t existing = 0;
        if (providers[i].list(providers[i].context, ports, PXA_PERIPHERALS_MAX_PORTS, &existing) != PXA_STATUS_OK ||
            existing > PXA_PERIPHERALS_MAX_PORTS) return false;
        total += existing;
        if (total > PXA_PERIPHERALS_MAX_PORTS) return false;
        for (size_t a = 0; a < added_count; ++a) for (size_t b = 0; b < existing; ++b)
            if (added[a].id == ports[b].id || !std::strcmp(added[a].name, ports[b].name)) return false;
    }
    const pxa_peripherals_backend_t mux = {
        sizeof(pxa_peripherals_backend_t), nullptr, List, Module, Open, Io, Start, Poll, Cancel, Close, Ready,
    };
    if (!count && !pxa_esp_peripherals_set_backend(&mux)) return false;
    std::memcpy(&providers[count++], p, p->struct_size >= sizeof(*p) ? sizeof(*p) : PXA_PERIPHERALS_BACKEND_BASE_SIZE);
    return true;
}
