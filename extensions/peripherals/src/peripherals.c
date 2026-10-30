// SPDX-License-Identifier: Apache-2.0
#include <pxa_ext/peripherals.h>
#include <string.h>
#define MAGIC UINT32_C(0x50525031)
#define SCRATCH 1536u
typedef struct {
    struct pxa_peripherals_service *service;
    pxa_handle64_t handle;
    pxa_component_t component;
    pxa_authority_t authority;
    void *device;
    uint8_t opened;
} peripheral_resource_t;
typedef struct {
    pxa_request_token_t token;
    uint64_t identity, cookie;
    pxa_handle64_t handle;
    pxa_component_t component;
    uint16_t rx_size, opcode;
    peripheral_resource_t *resource;
    void *device;
} pending_t;
struct pxa_peripherals_service {
    uint32_t magic, epoch;
    pxa_runtime_t *runtime;
    pxa_permission_service_t *permissions;
    pxa_peripherals_backend_t backend;
    pxa_component_t foreground;
    uint32_t sequence;
    pxa_component_t negotiated[32];
    uint16_t negotiated_minor[32];
    peripheral_resource_t resources[PXA_PERIPHERALS_MAX_HANDLES];
    pending_t pending[PXA_PERIPHERALS_MAX_PENDING];
    pxa_peripheral_port_t ports[PXA_PERIPHERALS_MAX_PORTS];
    size_t port_count;
    uint8_t scratch[SCRATCH];
    uint8_t registered;
};
static uint32_t next_epoch;
static uint16_t minor_for(const pxa_peripherals_service_t *s, pxa_component_t c) {
    size_t i;
    for (i = 0; i < 32; ++i) if (s->negotiated[i] == c) return s->negotiated_minor[i];
    return 0;
}
static int valid(const pxa_peripherals_service_t *s) { return s && s->magic == MAGIC; }
static pxa_status_t normalize(pxa_status_t status) { return pxa_status_is_known(status) ? status : PXA_STATUS_IO_ERROR; }
size_t pxa_peripherals_service_workspace_size(void) { return sizeof(pxa_peripherals_service_t); }
pxa_status_t pxa_peripherals_service_init(void *workspace, size_t bytes, pxa_runtime_t *runtime,
    pxa_permission_service_t *permissions, const pxa_peripherals_backend_t *backend,
    pxa_peripherals_service_t **output) {
    pxa_peripherals_service_t *s = workspace;
    if (!output) return PXA_STATUS_INVALID_ARGUMENT;
    *output = NULL;
    if (!s || bytes < sizeof(*s) || (uintptr_t)s % sizeof(uint64_t) || !runtime || !permissions ||
        !backend || backend->struct_size < PXA_PERIPHERALS_BACKEND_BASE_SIZE || !backend->list || !backend->module ||
        !backend->open || !backend->io || !backend->start || !backend->poll || !backend->cancel ||
        !backend->close) return PXA_STATUS_INVALID_ARGUMENT;
    uint32_t epoch = __atomic_load_n(&next_epoch, __ATOMIC_RELAXED);
    do {
        if (epoch == UINT32_MAX) return PXA_STATUS_RESOURCE_LIMIT;
    } while (!__atomic_compare_exchange_n(&next_epoch, &epoch, epoch + 1, 0,
        __ATOMIC_RELAXED, __ATOMIC_RELAXED));
    memset(s, 0, sizeof(*s)); s->magic = MAGIC; s->epoch = epoch + 1;
    s->runtime = runtime; s->permissions = permissions;
    memcpy(&s->backend, backend, backend->struct_size >= sizeof(*backend) ?
        sizeof(*backend) : PXA_PERIPHERALS_BACKEND_BASE_SIZE);
    *output = s; return PXA_STATUS_OK;
}
static pxa_status_t list(pxa_peripherals_service_t *s) {
    size_t i, j;
    pxa_status_t status = normalize(s->backend.list(s->backend.context, s->ports,
        PXA_PERIPHERALS_MAX_PORTS, &s->port_count));
    if (status != PXA_STATUS_OK) return status;
    if (s->port_count > PXA_PERIPHERALS_MAX_PORTS) return PXA_STATUS_PROTOCOL_ERROR;
    for (i = 0; i < s->port_count; ++i) {
        const pxa_peripheral_port_t *p = &s->ports[i];
        if (!p->id || p->kind < 1 || p->kind > PXA_PERIPHERALS_VISION || !p->name[0] || !memchr(p->name, 0, sizeof(p->name)) ||
            (p->flags & ~7u)) return PXA_STATUS_PROTOCOL_ERROR;
        for (j = 0; j < strlen(p->name); ++j)
            if ((uint8_t)p->name[j] < 0x21 || (uint8_t)p->name[j] > 0x7e) return PXA_STATUS_PROTOCOL_ERROR;
        for (j = 0; j < i; ++j) if (p->id == s->ports[j].id || !strcmp(p->name, s->ports[j].name))
            return PXA_STATUS_PROTOCOL_ERROR;
    }
    return PXA_STATUS_OK;
}
static void close_resource(void *context) {
    peripheral_resource_t *r = context;
    pxa_peripherals_service_t *s = r->service;
    size_t i;
    for (i = 0; i < PXA_PERIPHERALS_MAX_PENDING; ++i) {
        pending_t *p = &s->pending[i];
        if (!p->token || p->resource != r || p->handle != r->handle) continue;
        if (p->opcode == PXA_PERIPHERALS_TRANSFER)
            s->backend.cancel(s->backend.context, p->device, p->cookie);
        if (pxa_request_identity(s->runtime, p->component, p->token) == p->identity)
            (void)pxa_request_complete(s->runtime, p->component, p->token, PXA_STATUS_CANCELLED, NULL, 0);
        memset(p, 0, sizeof(*p));
    }
    if (r->device) s->backend.close(s->backend.context, r->device);
    r->handle = 0; r->device = NULL; r->opened = 0;
}
static int32_t resource_io(void *context, uint32_t op, uint8_t *data, size_t size) {
    peripheral_resource_t *r = context;
    if (!r->opened) return PXA_STATUS_WOULD_BLOCK;
    if (op != PXA_IO_READ && op != PXA_IO_WRITE) return PXA_STATUS_UNSUPPORTED;
    if (!data || !size || size > PXA_PERIPHERALS_MAX_TRANSFER) return PXA_STATUS_INVALID_ARGUMENT;
    int32_t result = r->service->backend.io(r->service->backend.context, r->device, op, data, size);
    if ((result >= 0 && (size_t)result > size) || (result < 0 && !pxa_status_is_known(result)))
        return PXA_STATUS_IO_ERROR;
    return result;
}
static const pxa_resource_ops_t resource_ops = {sizeof(pxa_resource_ops_t), resource_io};
static pxa_status_t open_resource(pxa_peripherals_service_t *s, pxa_component_t component,
    const pxa_message_view_t *m, size_t *result_size, int *deferred) {
    static const uint8_t permission[] = PXA_PERIPHERALS_PERMISSION;
    peripheral_resource_t *r = NULL;
    pending_t *pending = NULL;
    pxa_authority_t authority;
    pxa_resource_t resource;
    pxa_status_t status;
    pxa_bytes_t payload = m->payload;
    const pxa_peripheral_port_t *port = NULL;
    size_t i;
    if (payload.size != 20 || pxa_read_u16(payload.data+10) || !(pxa_read_u64(payload.data+12) >> 32))
        return PXA_STATUS_INVALID_ARGUMENT;
    if (!s->foreground || component != s->foreground) return PXA_STATUS_DENIED;
    status = list(s); if (status != PXA_STATUS_OK) return status;
    for (i = 0; i < s->port_count; ++i) if (s->ports[i].id == pxa_read_u16(payload.data)) port = &s->ports[i];
    if (!port) return PXA_STATUS_NOT_FOUND;
    if ((port->kind > PXA_PERIPHERALS_SLOT && minor_for(s, component) < 2) ||
        (port->kind > PXA_PERIPHERALS_RGB && minor_for(s, component) < 3))
        return PXA_STATUS_UNSUPPORTED;
    if (port->flags & PXA_PERIPHERALS_RESERVED) return PXA_STATUS_UNAVAILABLE;
    status = pxa_permission_resolve64(s->permissions, component, pxa_read_u64(payload.data+12),
        (pxa_bytes_t){permission, sizeof(permission)-1},
        (pxa_bytes_t){(const uint8_t *)port->name, strlen(port->name)}, &authority);
    if (status != PXA_STATUS_OK) return status;
    for (i = 0; i < PXA_PERIPHERALS_MAX_HANDLES; ++i) if (!s->resources[i].handle) { r = &s->resources[i]; break; }
    if (!r) return PXA_STATUS_RESOURCE_LIMIT;
    if (s->backend.ready) {
        for (i = 0; i < PXA_PERIPHERALS_MAX_PENDING; ++i)
            if (!s->pending[i].token) { pending = &s->pending[i]; break; }
        if (!pending) return PXA_STATUS_RESOURCE_LIMIT;
    }
    r->service = s; r->component = component; r->authority = authority;
    r->device = NULL; r->opened = 0;
    // Bind authority and reserve a Core handle before native side effects.
    resource = (pxa_resource_t){r, &resource_ops, close_resource};
    status = pxa_handle_open64(s->runtime, component, PXA_PERIPHERALS_RESOURCE_TYPE, authority, &resource, &r->handle);
    if (status != PXA_STATUS_OK) return status;
    status = normalize(s->backend.open(s->backend.context, ((uint64_t)s->epoch << 32) | component,
        port->id, pxa_read_u16(payload.data+2), pxa_read_u32(payload.data+4),
        pxa_read_u16(payload.data+8), &r->device));
    if (status == PXA_STATUS_OK && !r->device) status = PXA_STATUS_PROTOCOL_ERROR;
    if (status != PXA_STATUS_OK) {
        (void)pxa_handle_close64(s->runtime, component, r->handle); return status;
    }
    if (s->backend.ready) {
        status = normalize(s->backend.ready(s->backend.context, r->device));
        if (status == PXA_STATUS_WOULD_BLOCK) {
            *pending = (pending_t){m->request_token, pxa_request_identity(s->runtime, component, m->request_token),
                0, r->handle, component, 0, PXA_PERIPHERALS_OPEN, r, r->device};
            *deferred = 1; return PXA_STATUS_OK;
        }
        if (status != PXA_STATUS_OK) {
            (void)pxa_handle_close64(s->runtime, component, r->handle); return status;
        }
    }
    r->opened = 1;
    pxa_write_u64(s->scratch, r->handle); *result_size = 8;
    return PXA_STATUS_OK;
}
static pxa_status_t command(pxa_peripherals_service_t *s, pxa_component_t component,
    const pxa_message_view_t *m, size_t *result_size, int *deferred) {
    static const uint8_t identity[] = PXA_PERIPHERALS_IDENTITY;
    pxa_status_t status;
    *result_size = 0;
    if (m->opcode == PXA_PERIPHERALS_HELLO) {
        if (m->payload.size != 24) return PXA_STATUS_INVALID_ARGUMENT;
        const uint32_t minimum = ((uint32_t)pxa_read_u16(m->payload.data+16) << 16) |
            pxa_read_u16(m->payload.data+18);
        const uint32_t maximum = ((uint32_t)pxa_read_u16(m->payload.data+20) << 16) |
            pxa_read_u16(m->payload.data+22);
        const uint32_t selected = maximum >= PXA_PERIPHERALS_MINOR ? PXA_PERIPHERALS_MINOR : maximum;
        if (memcmp(m->payload.data, identity, 16) || selected < 1 || minimum > selected)
            return PXA_STATUS_UNSUPPORTED;
        size_t i;
        for (i = 0; i < 32; ++i) if (s->negotiated[i] == component) break;
        if (i == 32) for (i = 0; i < 32; ++i) if (!s->negotiated[i]) { s->negotiated[i] = component; break; }
        if (i == 32) return PXA_STATUS_RESOURCE_LIMIT;
        s->negotiated_minor[i] = (uint16_t)selected;
        memcpy(s->scratch, identity, 16); pxa_write_u16(s->scratch+16, 0); pxa_write_u16(s->scratch+18, (uint16_t)selected);
        *result_size = 20; return PXA_STATUS_OK;
    }
    if (m->opcode == PXA_PERIPHERALS_LIST) {
        size_t i;
        if (m->payload.size) return PXA_STATUS_INVALID_ARGUMENT;
        status = list(s); if (status != PXA_STATUS_OK) return status;
        uint16_t visible = 0;
        *result_size = 2;
        for (i = 0; i < s->port_count; ++i) {
            const pxa_peripheral_port_t *p = &s->ports[i];
            if ((p->kind > PXA_PERIPHERALS_SLOT && minor_for(s, component) < 2) ||
                (p->kind > PXA_PERIPHERALS_RGB && minor_for(s, component) < 3)) continue;
            size_t length = strlen(p->name), offset = *result_size;
            pxa_write_u16(s->scratch+offset, p->id); pxa_write_u16(s->scratch+offset+2, p->kind);
            pxa_write_u32(s->scratch+offset+4, p->flags); pxa_write_u16(s->scratch+offset+8, (uint16_t)length);
            memcpy(s->scratch+offset+10, p->name, length); *result_size += 10 + length;
            ++visible;
        }
        pxa_write_u16(s->scratch, visible);
        return PXA_STATUS_OK;
    }
    if (m->opcode == PXA_PERIPHERALS_MODULE_INFO) {
        pxa_peripheral_module_t info = {0};
        if (m->payload.size != 2) return PXA_STATUS_INVALID_ARGUMENT;
        status = normalize(s->backend.module(s->backend.context, pxa_read_u16(m->payload.data), &info));
        if (status != PXA_STATUS_OK) return status;
        if ((info.flags & ~31u) || ((info.flags & 2u) && !(info.flags & 1u)) ||
            info.name[31]) return PXA_STATUS_PROTOCOL_ERROR;
        pxa_write_u32(s->scratch, info.generation); pxa_write_u32(s->scratch+4, info.flags);
        pxa_write_u16(s->scratch+8, info.board_type); pxa_write_u16(s->scratch+10, info.board_id);
        pxa_write_u16(s->scratch+12, info.hardware_version); pxa_write_u16(s->scratch+14, info.vendor_id);
        pxa_write_u32(s->scratch+16, info.serial_number); memcpy(s->scratch+20, info.name, 32);
        *result_size = 52; return PXA_STATUS_OK;
    }
    if (m->opcode == PXA_PERIPHERALS_OPEN) return open_resource(s, component, m, result_size, deferred);
    return PXA_STATUS_UNSUPPORTED;
}
static pxa_status_t transfer(pxa_peripherals_service_t *s, pxa_component_t component,
    const pxa_message_view_t *m) {
    pending_t *pending = NULL; pxa_resource_t resource; peripheral_resource_t *r;
    pxa_status_t status; uint16_t tx, rx; size_t i;
    if (m->payload.size < 12) return PXA_STATUS_INVALID_ARGUMENT;
    tx = pxa_read_u16(m->payload.data+8); rx = pxa_read_u16(m->payload.data+10);
    if (tx > 256 || rx > 256 || (!tx && !rx) || m->payload.size != 12u + tx)
        return PXA_STATUS_INVALID_ARGUMENT;
    status = pxa_handle_get64(s->runtime, component, pxa_read_u64(m->payload.data),
        PXA_PERIPHERALS_RESOURCE_TYPE, &resource);
    if (status != PXA_STATUS_OK) return status;
    r = resource.context;
    if (!r || r->service != s) return PXA_STATUS_DENIED;
    if (!r->opened) return PXA_STATUS_WOULD_BLOCK;
    for (i = 0; i < PXA_PERIPHERALS_MAX_PENDING; ++i) if (!s->pending[i].token) { pending = &s->pending[i]; break; }
    if (!pending || s->sequence == UINT32_MAX) return PXA_STATUS_RESOURCE_LIMIT;
    const uint64_t cookie = ((uint64_t)s->epoch << 32) | ++s->sequence;
    status = normalize(s->backend.start(s->backend.context, r->device, cookie, m->payload.data+12, tx, rx));
    if (status != PXA_STATUS_OK) return status;
    *pending = (pending_t){m->request_token, pxa_request_identity(s->runtime, component, m->request_token),
        cookie, r->handle, component, rx, PXA_PERIPHERALS_TRANSFER, r, r->device};
    return PXA_STATUS_OK;
}
static pxa_status_t control(void *context, pxa_runtime_t *runtime, pxa_component_t component,
    const pxa_message_view_t *m) {
    pxa_peripherals_service_t *s = context;
    int deferred = 0;
    uint16_t profile; size_t bytes = 0; pxa_status_t status, completion;
    if (!valid(s) || !m || !m->request_token) return PXA_STATUS_INVALID_ARGUMENT;
    if (m->opcode < 1 || m->opcode > 5) return PXA_STATUS_UNSUPPORTED;
    status = pxa_component_wire_profile(runtime, component, &profile);
    if (status != PXA_STATUS_OK || profile != 1) return PXA_STATUS_UNSUPPORTED;
    status = pxa_request_begin_reserved(runtime, component, m->request_token,
        PXA_PERIPHERALS_SERVICE_ID, m->opcode, 0, m->opcode == PXA_PERIPHERALS_LIST ? SCRATCH : 256);
    if (status != PXA_STATUS_OK) return status;
    status = pxa_request_commit(runtime, component, m->request_token);
    if (status == PXA_STATUS_OK && m->opcode != PXA_PERIPHERALS_HELLO) {
        size_t i;
        for (i = 0; i < 32 && s->negotiated[i] != component; ++i) {}
        if (i == 32) status = PXA_STATUS_BAD_STATE;
    }
    if (status == PXA_STATUS_OK && m->opcode == PXA_PERIPHERALS_TRANSFER) {
        status = transfer(s, component, m);
        if (status == PXA_STATUS_OK) return PXA_STATUS_OK;
    } else if (status == PXA_STATUS_OK) status = command(s, component, m, &bytes, &deferred);
    if (status == PXA_STATUS_OK && deferred) return PXA_STATUS_OK;
    completion = pxa_request_complete(runtime, component, m->request_token, status,
        status == PXA_STATUS_OK ? s->scratch : NULL, status == PXA_STATUS_OK ? bytes : 0);
    if (completion != PXA_STATUS_OK) {
        if (status == PXA_STATUS_OK && m->opcode == PXA_PERIPHERALS_OPEN)
            (void)pxa_handle_close64(runtime, component, pxa_read_u64(s->scratch));
        (void)pxa_request_cancel(runtime, component, m->request_token);
    }
    return PXA_STATUS_OK;
}
static void stopped(void *context, pxa_runtime_t *runtime, pxa_component_t component) {
    pxa_peripherals_service_t *s = context;
    size_t i; (void)runtime;
    for (i = 0; i < 32; ++i) if (s->negotiated[i] == component) s->negotiated[i] = 0;
    if (s->foreground == component) s->foreground = 0;
}
pxa_status_t pxa_peripherals_service_register(pxa_peripherals_service_t *s) {
    pxa_service_ops_t ops = {0}; pxa_status_t status;
    if (!valid(s)) return PXA_STATUS_INVALID_ARGUMENT;
    if (s->registered) return PXA_STATUS_BAD_STATE;
    ops.struct_size = sizeof(ops); ops.service_id = PXA_PERIPHERALS_SERVICE_ID;
    ops.major = PXA_PERIPHERALS_MAJOR; ops.minor = PXA_PERIPHERALS_MINOR; ops.context = s; ops.control = control; ops.component_stopped = stopped;
    status = pxa_service_register(s->runtime, &ops);
    if (status == PXA_STATUS_OK) s->registered = 1;
    return status;
}
void pxa_peripherals_service_set_foreground(pxa_peripherals_service_t *s, pxa_component_t component) {
    size_t i;
    if (!valid(s)) return;
    for (i = 0; i < PXA_PERIPHERALS_MAX_HANDLES; ++i) {
        peripheral_resource_t *r = &s->resources[i];
        if (r->handle && r->component != component) (void)pxa_handle_close64(s->runtime, r->component, r->handle);
    }
    s->foreground = component;
}
int pxa_peripherals_service_pending(const pxa_peripherals_service_t *s) {
    size_t i; if (!valid(s)) return 0;
    for (i = 0; i < PXA_PERIPHERALS_MAX_PENDING; ++i) if (s->pending[i].token) return 1;
    return 0;
}
void pxa_peripherals_service_poll(pxa_peripherals_service_t *s) {
    size_t i;
    if (!valid(s)) return;
    for (i = 0; i < PXA_PERIPHERALS_MAX_PENDING; ++i) {
        pending_t *p = &s->pending[i], done;
        uint8_t data[256] = {0}; size_t bytes = sizeof(data); pxa_status_t status, completion;
        if (!p->token) continue;
        if (p->resource->handle != p->handle ||
            pxa_request_identity(s->runtime, p->component, p->token) != p->identity) {
            if (p->opcode == PXA_PERIPHERALS_TRANSFER)
                s->backend.cancel(s->backend.context, p->device, p->cookie);
            status = PXA_STATUS_CANCELLED; bytes = 0;
        } else if (p->opcode == PXA_PERIPHERALS_OPEN)
            status = normalize(s->backend.ready(s->backend.context, p->device));
        else status = normalize(s->backend.poll(s->backend.context, p->device, p->cookie, data, &bytes));
        if (status == PXA_STATUS_WOULD_BLOCK) continue;
        done = *p;
        memset(p, 0, sizeof(*p)); // Detach before closing a handle invokes its callback.
        if (done.opcode == PXA_PERIPHERALS_OPEN) {
            bytes = 0;
            if (status == PXA_STATUS_OK) {
                done.resource->opened = 1; pxa_write_u64(data, done.handle); bytes = 8;
            } else if (done.resource->handle == done.handle)
                (void)pxa_handle_close64(s->runtime, done.component, done.handle);
        } else if (status == PXA_STATUS_OK && bytes != done.rx_size) {
            status = PXA_STATUS_PROTOCOL_ERROR; bytes = 0;
        }
        if (pxa_request_identity(s->runtime, done.component, done.token) != done.identity) continue;
        completion = pxa_request_complete(s->runtime, done.component, done.token, status,
            status == PXA_STATUS_OK ? data : NULL, status == PXA_STATUS_OK ? bytes : 0);
        if (completion != PXA_STATUS_OK && done.opcode == PXA_PERIPHERALS_OPEN && status == PXA_STATUS_OK)
            (void)pxa_handle_close64(s->runtime, done.component, done.handle);
    }
}
