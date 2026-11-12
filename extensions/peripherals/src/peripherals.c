// SPDX-License-Identifier: Apache-2.0
#include <pxa_ext/peripherals.h>
#include <string.h>
#include <pxa_ext/vision_frame.h>
#include <pxa/fs.h>
#define MAGIC UINT32_C(0x50525031)
#define SCRATCH 1536u
#define FILE_FRAME_READ_BYTES 1024u
typedef struct {
    struct pxa_peripherals_service *service;
    pxa_handle64_t handle;
    pxa_component_t component;
    pxa_authority_t authority;
    void *device;
    uint8_t opened, jpeg, preview_pending;
    pxa_handle64_t frame;
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
typedef struct {
    struct pxa_peripherals_service *service;
    pxa_handle64_t handle, parent;
    pxa_component_t component;
    uint8_t header[PXA_VISION_HEADER_BYTES];
    size_t header_used;
    uint32_t remaining;
    uint8_t complete, failed;
    uint8_t file_slice;
    uint64_t file_offset, file_cookie, next_file_cookie;
    uint16_t file_size, file_used, file_requested;
    uint8_t file_packet[PXA_FS_READ_AT_HEADER_BYTES + FILE_FRAME_READ_BYTES];
} frame_stream_t;
struct pxa_peripherals_service {
    uint32_t magic, epoch;
    pxa_runtime_t *runtime;
    pxa_permission_service_t *permissions;
    pxa_assets_service_t *assets;
    pxa_peripherals_backend_t backend;
    pxa_component_t foreground;
    uint32_t sequence;
    pxa_component_t negotiated[32];
    uint16_t negotiated_minor[32];
    peripheral_resource_t resources[PXA_PERIPHERALS_MAX_HANDLES];
    pending_t pending[PXA_PERIPHERALS_MAX_PENDING];
    frame_stream_t frames[PXA_PERIPHERALS_MAX_HANDLES];
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
    memcpy(&s->backend, backend, PXA_PERIPHERALS_BACKEND_BASE_SIZE);
    if (backend->struct_size >= PXA_PERIPHERALS_BACKEND_READY_SIZE) s->backend.ready = backend->ready;
    if (backend->struct_size >= PXA_PERIPHERALS_BACKEND_PROFILES_SIZE) s->backend.camera_profiles = backend->camera_profiles;
    if (backend->struct_size >= sizeof(*backend)) s->backend.camera_preview = backend->camera_preview;
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
    r->handle = 0; r->device = NULL; r->opened = 0; r->frame = 0; r->preview_pending = 0;
}
static int32_t resource_io(void *context, uint32_t op, uint8_t *data, size_t size) {
    peripheral_resource_t *r = context;
    if (r->frame || r->preview_pending) return PXA_STATUS_BUSY;
    if (!r->opened) return PXA_STATUS_WOULD_BLOCK;
    if (op != PXA_IO_READ && op != PXA_IO_WRITE) return PXA_STATUS_UNSUPPORTED;
    if (!data || !size || size > PXA_PERIPHERALS_MAX_TRANSFER) return PXA_STATUS_INVALID_ARGUMENT;
    int32_t result = r->service->backend.io(r->service->backend.context, r->device, op, data, size);
    if ((result >= 0 && (size_t)result > size) || (result < 0 && !pxa_status_is_known(result)))
        return PXA_STATUS_IO_ERROR;
    return result;
}
static const pxa_resource_ops_t resource_ops = {sizeof(pxa_resource_ops_t), resource_io};
static peripheral_resource_t *frame_parent(frame_stream_t *frame) {
    pxa_resource_t resource;
    if (pxa_handle_get64(frame->service->runtime, frame->component, frame->parent,
            PXA_PERIPHERALS_RESOURCE_TYPE, &resource) != PXA_STATUS_OK) return NULL;
    peripheral_resource_t *parent = resource.context;
    return parent->frame == frame->handle && parent->opened ? parent : NULL;
}
static void close_frame(void *context) {
    frame_stream_t *frame = context;
    if (frame->file_slice) {
        pxa_resource_t source;
        if (frame->file_cookie && pxa_handle_get64(frame->service->runtime, frame->component,
                frame->parent, PXA_RESOURCE_FILE, &source) == PXA_STATUS_OK) {
            const pxa_resource_ops_t *operations = source.operations;
            uint8_t packet[8];
            pxa_write_u64(packet, frame->file_cookie);
            if (operations && operations->struct_size >= sizeof(*operations) && operations->io)
                (void)operations->io(source.context, PXA_FS_IO_CANCEL_READ_AT, packet, sizeof(packet));
        }
        frame->handle = 0;
        return;
    }
    peripheral_resource_t *parent = frame_parent(frame);
    const pxa_handle64_t parent_handle = frame->parent;
    const pxa_component_t component = frame->component;
    const uint8_t complete = frame->complete;
    frame->handle = 0;
    if (parent) {
        parent->frame = 0;
        if (!complete)
            (void)pxa_handle_close64(frame->service->runtime, component, parent_handle);
    }
}
static int32_t read_file_frame(frame_stream_t *frame, uint8_t *output, size_t capacity) {
    pxa_peripherals_service_t *s = frame->service;
    pxa_resource_t source;
    pxa_status_t status = pxa_handle_get64(s->runtime, frame->component, frame->parent,
                                          PXA_RESOURCE_FILE, &source);
    if (status) return status;
    const pxa_resource_ops_t *operations = source.operations;
    if (!operations || operations->struct_size < sizeof(*operations) || !operations->io)
        return PXA_STATUS_UNSUPPORTED;
    if (frame->failed) return PXA_STATUS_PROTOCOL_ERROR;
    if (frame->complete) return 0;
    if (frame->file_used == frame->file_size) {
        if (!frame->file_cookie) {
            frame->file_cookie = frame->next_file_cookie++;
            frame->file_requested = (uint16_t)(frame->remaining < FILE_FRAME_READ_BYTES ? frame->remaining : FILE_FRAME_READ_BYTES);
            pxa_write_u64(frame->file_packet, frame->file_offset);
            pxa_write_u64(frame->file_packet+8, frame->file_cookie);
            pxa_write_u32(frame->file_packet+16, frame->file_requested);
            pxa_write_u32(frame->file_packet+20, 0);
        }
        int32_t count = operations->io(source.context, PXA_FS_IO_READ_AT, frame->file_packet,
                                      PXA_FS_READ_AT_HEADER_BYTES + frame->file_requested);
        if (count < 0) return normalize(count);
        if (!count || count > frame->file_requested) {
            frame->failed = 1; return PXA_STATUS_PROTOCOL_ERROR;
        }
        frame->file_cookie = 0;
        frame->file_offset += (uint32_t)count;
        frame->file_size = (uint16_t)count; frame->file_used = 0;
    }
    size_t count = frame->file_size - frame->file_used;
    if (count > capacity) count = capacity;
    memcpy(output, frame->file_packet + PXA_FS_READ_AT_HEADER_BYTES + frame->file_used, count);
    frame->file_used += (uint16_t)count; frame->remaining -= (uint32_t)count;
    frame->complete = frame->remaining == 0;
    return (int32_t)count;
}
static int32_t read_frame(void *context, uint32_t operation, uint8_t *output, size_t capacity) {
    frame_stream_t *frame = context;
    if (operation != PXA_IO_READ) return PXA_STATUS_UNSUPPORTED;
    if (!capacity || !output) return PXA_STATUS_INVALID_ARGUMENT;
    if (frame->file_slice) return read_file_frame(frame, output, capacity);
    peripheral_resource_t *parent = frame_parent(frame);
    if (!parent) return PXA_STATUS_UNAVAILABLE;
    if (frame->failed) return PXA_STATUS_PROTOCOL_ERROR;
    if (frame->complete) return 0;
    pxa_peripherals_service_t *s = frame->service;
    if (frame->header_used < sizeof(frame->header)) {
        size_t size = sizeof(frame->header) - frame->header_used;
        int32_t count = s->backend.io(s->backend.context, parent->device, PXA_IO_READ,
                                    frame->header + frame->header_used, size);
        if (count < 0) return normalize(count);
        if (!count || (size_t)count > size) goto malformed;
        frame->header_used += (size_t)count;
        if (frame->header_used < sizeof(frame->header)) return PXA_STATUS_WOULD_BLOCK;
        pxa_vision_frame_info_t info;
        if (!pxa_vision_decode(frame->header, &info) || info.format != PXA_VISION_JPEG ||
            info.size > PXA_VISION_JPEG_LIMIT(minor_for(s, frame->component)))
            goto malformed;
        frame->remaining = info.size;
    }
    // Coalesce native 256-byte transfers into one bounded Core STREAM read.
    // The call budget also bounds backends that return very short fragments.
    const unsigned calls = minor_for(s, frame->component) >= 8 ? 64 : 16;
    const size_t window = calls * PXA_PERIPHERALS_MAX_TRANSFER;
    const size_t limit = capacity < window ? capacity : window;
    size_t copied = 0;
    for (unsigned call = 0; call < calls && copied < limit && frame->remaining; ++call) {
        size_t size = limit - copied;
        if (size > PXA_PERIPHERALS_MAX_TRANSFER) size = PXA_PERIPHERALS_MAX_TRANSFER;
        if (size > frame->remaining) size = frame->remaining;
        int32_t count = s->backend.io(s->backend.context, parent->device, PXA_IO_READ, output + copied, size);
        if (count == PXA_STATUS_WOULD_BLOCK) return copied ? (int32_t)copied : count;
        if (count < 0) { frame->failed = 1; return normalize(count); }
        if (!count || (size_t)count > size) goto malformed;
        frame->remaining -= (uint32_t)count;
        copied += (size_t)count;
    }
    frame->complete = frame->remaining == 0;
    return (int32_t)copied;
malformed:
    frame->failed = 1;
    return PXA_STATUS_PROTOCOL_ERROR;
}
static const pxa_resource_ops_t frame_ops = {sizeof(pxa_resource_ops_t), read_frame};
static pxa_status_t open_file_frame(pxa_peripherals_service_t *s, pxa_component_t component,
    const pxa_message_view_t *m, size_t *result_size) {
    if (minor_for(s, component) < 6) return PXA_STATUS_UNSUPPORTED;
    if (component != s->foreground) return PXA_STATUS_DENIED;
    if (m->payload.size != 28) return PXA_STATUS_INVALID_ARGUMENT;
    const uint64_t parent = pxa_read_u64(m->payload.data), offset = pxa_read_u64(m->payload.data+8);
    const uint32_t length = pxa_read_u32(m->payload.data+16);
    const uint64_t first_cookie = pxa_read_u64(m->payload.data+20);
    if (!length || length > PXA_VISION_JPEG_LIMIT(minor_for(s, component)) || offset > UINT64_MAX-length ||
        !first_cookie || first_cookie > UINT64_MAX-length) return PXA_STATUS_INVALID_ARGUMENT;
    pxa_resource_t source;
    pxa_status_t status = pxa_handle_get64(s->runtime, component, parent, PXA_RESOURCE_FILE, &source);
    if (status) return status;
    const pxa_resource_ops_t *operations = source.operations;
    if (!operations || operations->struct_size < sizeof(*operations) || !operations->io)
        return PXA_STATUS_UNSUPPORTED;
    size_t index;
    for (index=0; index<PXA_PERIPHERALS_MAX_HANDLES && s->frames[index].handle; ++index) {}
    if (index == PXA_PERIPHERALS_MAX_HANDLES) return PXA_STATUS_RESOURCE_LIMIT;
    frame_stream_t *frame = &s->frames[index]; memset(frame,0,sizeof(*frame));
    frame->service=s; frame->component=component; frame->parent=parent;
    frame->file_slice=1; frame->file_offset=offset; frame->remaining=length;
    frame->next_file_cookie=first_cookie;
    pxa_resource_t resource={frame,&frame_ops,close_frame};
    status=pxa_handle_open64(s->runtime,component,PXA_RESOURCE_STREAM,0,&resource,&frame->handle);
    if (status) return status;
    pxa_write_u64(s->scratch,frame->handle);*result_size=8;return PXA_STATUS_OK;
}
static pxa_status_t open_frame(pxa_peripherals_service_t *s, pxa_component_t component,
                               const pxa_message_view_t *m, size_t *result_size) {
    if (minor_for(s, component) < 4) return PXA_STATUS_UNSUPPORTED;
    if (!s->foreground || component != s->foreground) return PXA_STATUS_DENIED;
    if (m->payload.size != 8) return PXA_STATUS_INVALID_ARGUMENT;
    pxa_resource_t resource;
    const pxa_handle64_t handle = pxa_read_u64(m->payload.data);
    pxa_status_t status = pxa_handle_get64(s->runtime, component, handle,
                                         PXA_PERIPHERALS_RESOURCE_TYPE, &resource);
    if (status) return status;
    peripheral_resource_t *parent = resource.context;
    if (parent->service != s) return PXA_STATUS_DENIED;
    if (!parent->opened || !parent->jpeg) return PXA_STATUS_UNSUPPORTED;
    if (parent->frame || parent->preview_pending) return PXA_STATUS_BUSY;
    size_t index;
    for (index = 0; index < PXA_PERIPHERALS_MAX_HANDLES; ++index)
        if (!s->frames[index].handle) break;
    if (index == PXA_PERIPHERALS_MAX_HANDLES) return PXA_STATUS_RESOURCE_LIMIT;
    frame_stream_t *frame = &s->frames[index];
    memset(frame, 0, sizeof(*frame));
    frame->service = s; frame->component = component; frame->parent = handle;
    resource = (pxa_resource_t){frame, &frame_ops, close_frame};
    status = pxa_handle_open64(s->runtime, component, PXA_RESOURCE_STREAM,
                               parent->authority, &resource, &frame->handle);
    if (status) return status;
    parent->frame = frame->handle;
    pxa_write_u64(s->scratch, frame->handle); *result_size = 8;
    return PXA_STATUS_OK;
}
void pxa_peripherals_service_set_assets(pxa_peripherals_service_t *s, pxa_assets_service_t *assets) {
    if (valid(s)) s->assets = assets;
}
static pxa_status_t preview_result(peripheral_resource_t *parent, uint8_t *output, size_t *result_size) {
    pxa_peripherals_service_t *s = parent->service;
    pxa_raster_asset_t *image = NULL;
    *result_size = 0;
    pxa_status_t status = normalize(s->backend.camera_preview(s->backend.context, parent->device, &image));
    if (status != PXA_STATUS_OK) {
        pxa_raster_asset_release(image);
        return status;
    }
    pxa_asset_object_view_t view;
    pxa_asset_object_view(image, &view);
    const uint16_t minor = minor_for(s, parent->component);
    if (view.width > PXA_CAMERA_PREVIEW_EDGE(minor) || view.height > PXA_CAMERA_PREVIEW_EDGE(minor)) {
        pxa_raster_asset_release(image);
        // Legacy clients fall back to their bounded JPEG preview path.
        return minor < 8 ? PXA_STATUS_UNSUPPORTED : PXA_STATUS_PROTOCOL_ERROR;
    }
    pxa_handle64_t handle = 0;
    status = pxa_assets_publish_image(s->assets, parent->component, image, &handle);
    if (status) { pxa_raster_asset_release(image); return status; }
    pxa_write_u64(output, handle);
    output[8] = view.kind; output[9] = view.encoding;
    pxa_write_u16(output+10, 1);
    pxa_write_u16(output+12, view.width); pxa_write_u16(output+14, view.height);
    pxa_write_u32(output+16, view.bytes); pxa_write_u32(output+20, view.bytes);
    pxa_write_u32(output+24, (uint32_t)pxa_raster_asset_allocation_bytes(image));
    *result_size = 28;
    return PXA_STATUS_OK;
}
static pxa_status_t camera_preview(pxa_peripherals_service_t *s, pxa_component_t component,
    const pxa_message_view_t *m, size_t *result_size, int *deferred) {
    pxa_resource_t native;
    if (m->payload.size != 8) return PXA_STATUS_INVALID_ARGUMENT;
    if (component != s->foreground) return PXA_STATUS_DENIED;
    if (minor_for(s, component) < 7 || !s->assets || !s->backend.camera_preview)
        return PXA_STATUS_UNSUPPORTED;
    pxa_status_t status = pxa_handle_get64(s->runtime, component, pxa_read_u64(m->payload.data),
                                          PXA_PERIPHERALS_RESOURCE_TYPE, &native);
    if (status) return status;
    peripheral_resource_t *parent = native.context;
    if (!parent || parent->service != s) return PXA_STATUS_DENIED;
    if (!parent->opened || !parent->jpeg) return PXA_STATUS_UNSUPPORTED;
    if (parent->frame || parent->preview_pending) return PXA_STATUS_BUSY;
    pending_t *pending = NULL;
    for (size_t i = 0; i < PXA_PERIPHERALS_MAX_PENDING; ++i) {
        if (!s->pending[i].token) { pending = &s->pending[i]; break; }
    }
    if (!pending) return PXA_STATUS_RESOURCE_LIMIT;
    status = preview_result(parent, s->scratch, result_size);
    if (status != PXA_STATUS_WOULD_BLOCK) return status;
    *pending = (pending_t){m->request_token, pxa_request_identity(s->runtime, component, m->request_token),
        0, parent->handle, component, 0, PXA_PERIPHERALS_CAMERA_PREVIEW, parent, parent->device};
    parent->preview_pending = 1;
    *deferred = 1;
    return PXA_STATUS_OK;
}
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
    if (port->kind == PXA_PERIPHERALS_VISION && pxa_read_u32(payload.data+4) && minor_for(s, component) < 5)
        return PXA_STATUS_UNSUPPORTED;
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
    r->jpeg = port->kind == PXA_PERIPHERALS_VISION && !strcmp(port->name, "camera.jpeg");
    r->frame = 0;
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
static pxa_status_t camera_profiles(pxa_peripherals_service_t *s, pxa_component_t component,
    const pxa_message_view_t *m, size_t *result_size) {
    if (minor_for(s, component) < 5 || !s->backend.camera_profiles) return PXA_STATUS_UNSUPPORTED;
    if (m->payload.size != 2) return PXA_STATUS_INVALID_ARGUMENT;
    pxa_camera_profile_t profiles[PXA_CAMERA_MAX_PROFILES];
    size_t count = 0, visible = 0, fallback = 0;
    unsigned defaults = 0, visible_defaults = 0;
    pxa_status_t status = normalize(s->backend.camera_profiles(s->backend.context,
        pxa_read_u16(m->payload.data), profiles, PXA_CAMERA_MAX_PROFILES, &count));
    if (status) return status;
    if (!count || count > PXA_CAMERA_MAX_PROFILES) return PXA_STATUS_PROTOCOL_ERROR;
    const uint32_t limit = PXA_VISION_JPEG_LIMIT(minor_for(s, component));
    for (size_t i = 0; i < count; ++i) {
        const pxa_camera_profile_t *p = &profiles[i];
        if (!p->id || p->encoding != PXA_VISION_JPEG || p->quality > 100 ||
            (!p->width != !p->height) || (!p->width && !(p->flags & PXA_CAMERA_PROFILE_NATIVE_SIZE)) ||
            !p->max_frame_bytes || p->max_frame_bytes > PXA_VISION_MAX_JPEG_HD || (p->flags & ~3u))
            return PXA_STATUS_PROTOCOL_ERROR;
        for (size_t j = 0; j < i; ++j) if (profiles[j].id == p->id) return PXA_STATUS_PROTOCOL_ERROR;
        defaults += !!(p->flags & PXA_CAMERA_PROFILE_DEFAULT);
        if (p->max_frame_bytes > limit) continue;
        if (p->flags & PXA_CAMERA_PROFILE_NATIVE_SIZE) fallback = visible;
        visible_defaults += !!(p->flags & PXA_CAMERA_PROFILE_DEFAULT);
        uint8_t *out = s->scratch + 2 + visible++ * 24;
        pxa_write_u32(out, p->id); pxa_write_u16(out+4, p->encoding);
        pxa_write_u16(out+6, p->width); pxa_write_u16(out+8, p->height); pxa_write_u16(out+10, p->quality);
        pxa_write_u32(out+12, p->max_frame_bytes); pxa_write_u32(out+16, p->period_us); pxa_write_u32(out+20, p->flags);
    }
    if (defaults != 1) return PXA_STATUS_PROTOCOL_ERROR;
    if (!visible) return PXA_STATUS_UNSUPPORTED;
    if (!visible_defaults) {
        uint8_t *flags = s->scratch + 2 + fallback * 24 + 20;
        pxa_write_u32(flags, pxa_read_u32(flags) | PXA_CAMERA_PROFILE_DEFAULT);
    }
    pxa_write_u16(s->scratch, (uint16_t)visible); *result_size = 2 + visible * 24;
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
    if (m->opcode == PXA_PERIPHERALS_JPEG_FILE_FRAME) return open_file_frame(s, component, m, result_size);
    if (m->opcode == PXA_PERIPHERALS_CAMERA_PREVIEW) return camera_preview(s, component, m, result_size, deferred);
    if (m->opcode == PXA_PERIPHERALS_CAMERA_PROFILES) return camera_profiles(s, component, m, result_size);
    if (m->opcode == PXA_PERIPHERALS_FRAME_STREAM) return open_frame(s, component, m, result_size);
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
    if (m->opcode < 1 || m->opcode > PXA_PERIPHERALS_CAMERA_PREVIEW) return PXA_STATUS_UNSUPPORTED;
    status = pxa_component_wire_profile(runtime, component, &profile);
    if (status != PXA_STATUS_OK || profile != 1) return PXA_STATUS_UNSUPPORTED;
    status = pxa_request_begin_reserved(runtime, component, m->request_token,
        PXA_PERIPHERALS_SERVICE_ID, m->opcode, 0, (m->opcode == PXA_PERIPHERALS_LIST || m->opcode == PXA_PERIPHERALS_CAMERA_PROFILES) ? SCRATCH : 256);
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
        if (status == PXA_STATUS_OK && (m->opcode == PXA_PERIPHERALS_OPEN ||
            m->opcode == PXA_PERIPHERALS_FRAME_STREAM || m->opcode == PXA_PERIPHERALS_JPEG_FILE_FRAME ||
            m->opcode == PXA_PERIPHERALS_CAMERA_PREVIEW))
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
        frame_stream_t *frame = &s->frames[i];
        if (frame->handle && frame->component != component)
            (void)pxa_handle_close64(s->runtime, frame->component, frame->handle);
    }
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
        else if (p->opcode == PXA_PERIPHERALS_CAMERA_PREVIEW)
            status = preview_result(p->resource, data, &bytes);
        else status = normalize(s->backend.poll(s->backend.context, p->device, p->cookie, data, &bytes));
        if (status == PXA_STATUS_WOULD_BLOCK) continue;
        done = *p;
        memset(p, 0, sizeof(*p)); // Detach before closing a handle invokes its callback.
        if (done.opcode == PXA_PERIPHERALS_CAMERA_PREVIEW) {
            if (done.resource->handle == done.handle) {
                done.resource->preview_pending = 0;
                if (status == PXA_STATUS_CANCELLED)
                    (void)pxa_handle_close64(s->runtime, done.component, done.handle);
            }
        } else if (done.opcode == PXA_PERIPHERALS_OPEN) {
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
        if (completion != PXA_STATUS_OK && done.opcode == PXA_PERIPHERALS_CAMERA_PREVIEW && status == PXA_STATUS_OK)
            (void)pxa_handle_close64(s->runtime, done.component, pxa_read_u64(data));
    }
}
