// SPDX-License-Identifier: Apache-2.0
#include <pxa_ext/peripherals.h>
#include <pxa/haptics.h>
#include <pxa_ext/vision_frame.h>
#include <pxa/fs.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static pxa_runtime_t *runtime;
static pxa_component_t component, other;
static pxa_permission_service_t *permissions;
static pxa_request_token_t token = 1;
static uint8_t response[2048];
static pxa_message_view_t reply;
static unsigned file_closed, file_cancelled;
static int file_block;
static uint64_t file_cookie, file_retired_cookie = 100;
static const uint8_t file_bytes[] = "0123456789abcdefghijklmnop";
static int32_t file_io(void *ctx, uint32_t operation, uint8_t *packet, size_t size) {
    (void)ctx;
    if (operation == PXA_FS_IO_CANCEL_READ_AT) {
        assert(size == 8); ++file_cancelled;
        if (pxa_read_u64(packet) > file_retired_cookie) file_retired_cookie=pxa_read_u64(packet);
        file_cookie=0; return 0;
    }
    assert(operation == PXA_FS_IO_READ_AT && size >= 24);
    const uint64_t offset = pxa_read_u64(packet), cookie = pxa_read_u64(packet+8);
    const uint32_t count = pxa_read_u32(packet+16);
    assert(cookie && count && size == 24u+count && !pxa_read_u32(packet+20));
    if (file_block) { file_block=0; file_cookie=cookie; return PXA_STATUS_WOULD_BLOCK; }
    if (file_cookie) { assert(cookie == file_cookie); file_cookie=0; }
    if (cookie <= file_retired_cookie) return PXA_STATUS_CANCELLED;
    file_retired_cookie=cookie;
    size_t n=offset < sizeof(file_bytes)-1 ? sizeof(file_bytes)-1-(size_t)offset : 0;
    if(n>count)n=count;
    memcpy(packet+24,file_bytes+(n ? (size_t)offset : 0),n);
    return (int32_t)n;
}
static void file_close(void *context) { (void)context; ++file_closed; }
static const pxa_resource_ops_t file_ops = {sizeof(pxa_resource_ops_t), file_io};
static pxa_status_t load(void *ctx, pxa_bytes_t app, pxa_bytes_t name,
                         pxa_bytes_t scope, pxa_permission_decision_t *decision) {
    (void)ctx; (void)app; (void)name; (void)scope;
    *decision = PXA_PERMISSION_ALLOW; return PXA_STATUS_OK;
}
static pxa_status_t save(void *ctx, pxa_bytes_t app, pxa_bytes_t name,
                         pxa_bytes_t scope, pxa_permission_decision_t decision) {
    (void)ctx; (void)app; (void)name; (void)scope; (void)decision;
    return PXA_STATUS_OK;
}
static pxa_component_t create(uint64_t id) {
    pxa_component_t c;
    assert(pxa_component_create(runtime, id, &c) == PXA_STATUS_OK);
    assert(pxa_component_begin_start(runtime, c) == PXA_STATUS_OK);
    assert(pxa_component_set_wire_profile(runtime, c, 1) == PXA_STATUS_OK);
    assert(pxa_component_finish_start(runtime, c, PXA_STATUS_OK) == PXA_STATUS_OK);
    return c;
}
static void pop(pxa_component_t c) {
    size_t size;
    assert(pxa_event_pop(runtime, c, response, sizeof(response), &size) == PXA_STATUS_OK);
    assert(pxa_message_decode(response, size, sizeof(response), &reply) == PXA_STATUS_OK);
}
static pxa_status_t dispatch(pxa_component_t c, uint16_t sid, uint16_t op,
                             const void *data, size_t size) {
    pxa_status_t status;
    pxa_message_view_t message = {sid, op, token++, {data, size}};
    assert(pxa_component_begin_event(runtime, c) == PXA_STATUS_OK);
    status = pxa_runtime_control_view(runtime, c, &message);
    assert(pxa_component_finish_event(runtime, c, 1) == PXA_STATUS_OK);
    return status;
}
static pxa_status_t call(pxa_component_t c, uint16_t sid, uint16_t op,
                         const void *data, size_t size) {
    assert(dispatch(c, sid, op, data, size) == PXA_STATUS_OK);
    pop(c);
    assert(reply.service == sid && reply.opcode == op && reply.request_token == token-1);
    assert(reply.payload.size >= 4);
    return (pxa_status_t)pxa_read_u32(reply.payload.data);
}
static int32_t read_resource(pxa_component_t c, pxa_handle64_t h, uint8_t *data, size_t size) {
    assert(pxa_component_begin_event(runtime, c) == PXA_STATUS_OK);
    int32_t count = pxa_runtime_io64(runtime, c, h, PXA_IO_READ, data, size);
    assert(pxa_component_finish_event(runtime, c, 1) == PXA_STATUS_OK);
    return count;
}
static pxa_handle64_t acquire_named(pxa_component_t c, const char *name, const char *scope) {
    uint8_t packet[128];
    pxa_writer_t writer;
    pxa_writer_init(&writer, packet, sizeof(packet));
    assert(pxa_writer_record(&writer, 1, name, strlen(name)) == PXA_STATUS_OK);
    assert(pxa_writer_record(&writer, 2, scope, strlen(scope)) == PXA_STATUS_OK);
    assert(call(c, 11, PXA_PERMISSION_ACQUIRE, packet, writer.size) == PXA_STATUS_OK);
    assert(reply.payload.size == 12);
    return pxa_read_u64(reply.payload.data + 4);
}
static unsigned opens, closes, starts, cancels, stops, plays;
static uint64_t cookie;
static int ready;
static size_t backend_bytes=2;
static pxa_status_t backend_poll_status=PXA_STATUS_OK, backend_open_status=PXA_STATUS_OK;
static int backend_null_open;
static int camera_mode, frame_block;
static uint8_t frame_bytes[32768+PXA_VISION_HEADER_BYTES];
static size_t frame_offset, frame_chunk = 7, frame_length = 32, native_reads;
static size_t frame_block_after = SIZE_MAX;
static uint16_t listed_kind = PXA_PERIPHERALS_I2C;
static pxa_status_t port_list(void *ctx, pxa_peripheral_port_t *p, size_t capacity, size_t *count) {
    (void)ctx; assert(capacity >= 1); *count = 1;
    *p = (pxa_peripheral_port_t){3, listed_kind, 3, "expansion.i2c"};
    if (camera_mode) { p->kind = PXA_PERIPHERALS_VISION; strcpy(p->name, "camera.jpeg"); }
    return PXA_STATUS_OK;
}
static int invalid_profile, hd_profile;
static unsigned image_allocations;
static int preview_block;
static uint16_t preview_width = 2;
static pxa_status_t preview_error;
static void* image_allocate(void* context, size_t size) { (void)context; ++image_allocations; return malloc(size); }
static void image_free(void* context, void* pointer) { (void)context; --image_allocations; free(pointer); }
static pxa_status_t preview_image(void* context, void* device, pxa_raster_asset_t** output) {
    (void)context; assert(device==&opens); *output=NULL;
    if (preview_block) { --preview_block; return PXA_STATUS_WOULD_BLOCK; }
    if (preview_error) return preview_error;
    pxa_asset_info_t info={0}; uint8_t* pixels=NULL;
    info.kind=PXA_ASSET_IMAGE; info.encoding=PXA_ASSET_ENCODING_RGB565;
    info.width=preview_width; info.height=2; info.format_version=1; info.payload_offset=32;
    info.decoded_bytes=preview_width*4u; info.stored_bytes=32+info.decoded_bytes;
    pxa_status_t status=pxa_asset_object_create(&info,image_allocate,image_free,NULL,output,&pixels);
    if (!status) { memset(pixels,0x7c,info.decoded_bytes); pxa_raster_asset_finish_loading(*output); }
    return status;
}
static pxa_status_t profiles(void *ctx, uint16_t port, pxa_camera_profile_t *out, size_t capacity, size_t *count) {
    (void)ctx;
    if (port != 3 || !camera_mode) return PXA_STATUS_UNSUPPORTED;
    assert(capacity >= 2); *count = 2;
    out[0] = (pxa_camera_profile_t){1, PXA_VISION_JPEG, 320, 240, 75, 262144, 100000, PXA_CAMERA_PROFILE_DEFAULT};
    out[1] = (pxa_camera_profile_t){2, PXA_VISION_JPEG, 640, 480, 85, 262144, 300000, 0};
    if (hd_profile) {
        out[0].flags = 0;
        out[1] = (pxa_camera_profile_t){2, PXA_VISION_JPEG, 1920, 1080, 82,
            PXA_VISION_MAX_JPEG_HD, 40000, PXA_CAMERA_PROFILE_DEFAULT | PXA_CAMERA_PROFILE_NATIVE_SIZE};
    }
    if (invalid_profile) out[1].id = 1;
    return PXA_STATUS_OK;
}
static pxa_status_t module(void *ctx, uint16_t port, pxa_peripheral_module_t *out) {
    (void)ctx; if (port != 1) return PXA_STATUS_NOT_FOUND;
    *out = (pxa_peripheral_module_t){1, 3, 1, 42, 0x100, 1, 100, "test"}; return PXA_STATUS_OK;
}
static pxa_status_t port_open(void *ctx, uint64_t owner, uint16_t port, uint16_t mode,
    uint32_t parameter, uint16_t address, void **out) {
    (void)ctx; assert(owner >> 32); assert(port == 3 && (camera_mode ? mode == 7 && !parameter && !address : mode == 3 && parameter == 100000 && address == 0x20));
    ++opens; if (!backend_null_open) *out = &opens; return backend_open_status;
}
static int32_t io(void *ctx, void *device, uint32_t op, uint8_t *bytes, size_t size) {
    (void)ctx; (void)device;
    if (!camera_mode || op != PXA_IO_READ) return PXA_STATUS_UNSUPPORTED;
    if (frame_block) { frame_block = 0; return PXA_STATUS_WOULD_BLOCK; }
    if (frame_offset >= frame_block_after) { frame_block_after = SIZE_MAX; return PXA_STATUS_WOULD_BLOCK; }
    ++native_reads;
    size_t n = frame_length - frame_offset;
    if (n > size) n = size;
    if (n > frame_chunk) n = frame_chunk;
    memcpy(bytes, frame_bytes + frame_offset, n); frame_offset += n;
    return (int32_t)n;
}
static pxa_status_t start(void *ctx, void *device, uint64_t id, const uint8_t *tx, uint16_t n, uint16_t rx) {
    (void)ctx; assert(device == &opens && n == 1 && tx[0] == 42 && rx == 2 && id >> 32);
    ++starts; cookie = id; ready = 0; return PXA_STATUS_OK;
}
static pxa_status_t poll(void *ctx, void *device, uint64_t id, uint8_t *out, size_t *bytes) {
    (void)ctx; assert(device == &opens && id == cookie);
    if (!ready) return PXA_STATUS_WOULD_BLOCK;
    assert(*bytes >= 2); *bytes = backend_bytes; out[0] = 10; out[1] = 20; return backend_poll_status;
}
static void cancel(void *ctx, void *device, uint64_t id) {
    (void)ctx; assert(device == &opens && id >> 32); ++cancels;
}
static void close_port(void *ctx, void *device) { (void)ctx; assert(device == &opens); ++closes; }
static pxa_status_t play(void *ctx, uint32_t duration, uint16_t amplitude) {
    (void)ctx; assert(duration == 100 && amplitude == 1000); ++plays; return PXA_STATUS_OK;
}
static void stop(void *ctx) { (void)ctx; ++stops; }
int main(void) {
    pxa_runtime_limits_t limits; pxa_permission_config_t config = {0}; size_t size;
    pxa_peripherals_service_t *service; pxa_haptics_service_t *haptics;
    const pxa_permission_declaration_t declarations[] = {
        {{(const uint8_t *)PXA_PERIPHERALS_PERMISSION, sizeof(PXA_PERIPHERALS_PERMISSION)-1},
         {(const uint8_t *)"expansion.i2c", 13}, 0},
        {{(const uint8_t *)PXA_HAPTICS_PERMISSION, sizeof(PXA_HAPTICS_PERMISSION)-1},
         {(const uint8_t *)PXA_HAPTICS_SCOPE, sizeof(PXA_HAPTICS_SCOPE)-1}, 0},
        {{(const uint8_t *)PXA_PERIPHERALS_PERMISSION, sizeof(PXA_PERIPHERALS_PERMISSION)-1},
         {(const uint8_t *)"camera.jpeg", 11}, 0}};
    const pxa_peripherals_backend_t backend = {sizeof(backend), NULL, port_list, module,
        port_open, io, start, poll, cancel, close_port, NULL, profiles, preview_image};
    const pxa_haptics_backend_t motor = {sizeof(motor), NULL, 1, 1000, play, stop};
    pxa_runtime_limits_init(&limits); limits.max_services = 4; limits.max_components = 2;
    limits.max_requests = 8; limits.max_requests_per_component = 4;
    limits.max_handles = 12; limits.max_events = 10; limits.mailbox_capacity = 6;
    limits.event_block_count = 128; limits.event_block_size = 64;
    size = pxa_runtime_workspace_size(&limits); void *workspace = malloc(size); assert(workspace);
    assert(pxa_runtime_init(workspace, size, &limits, &runtime) == PXA_STATUS_OK);
    component = create(1); other = create(2);
    config.struct_size = sizeof(config); config.app_identity = (pxa_bytes_t){(const uint8_t *)"test", 4};
    config.declarations = declarations; config.declaration_count = 3; config.max_authorities = 3;
    config.store = (pxa_permission_store_t){sizeof(config.store), NULL, load, save};
    size = pxa_permission_service_workspace_size(&config); void *pw = malloc(size); assert(pw);
    assert(pxa_permission_service_init(pw, size, runtime, &config, &permissions) == PXA_STATUS_OK);
    assert(pxa_permission_policy_load(permissions) == PXA_STATUS_OK);
    assert(pxa_permission_service_register(permissions) == PXA_STATUS_OK);
    size = pxa_peripherals_service_workspace_size(); void *sw = malloc(size); assert(sw);
    assert(pxa_peripherals_service_init(sw, size, runtime, permissions, &backend, &service) == PXA_STATUS_OK);
    const pxa_memory_allocator_t image_memory={0}; const pxa_image_backend_t image_backend={0};
    pxa_assets_config_t images={0}; images.struct_size=sizeof(images);
    images.max_pending=images.max_pending_per_component=1;
    images.max_resources=images.max_resources_per_component=2;
    images.image_allocator=images.image_temporary_allocator=&image_memory; images.image_backend=&image_backend;
    size=pxa_assets_service_workspace_size(&images); void* aw=malloc(size); assert(aw);
    pxa_assets_service_t* assets=NULL;
    assert(!pxa_assets_service_init(aw,size,runtime,&images,&assets));
    assert(!pxa_assets_service_register(assets)); pxa_peripherals_service_set_assets(service,assets);
    assert(pxa_peripherals_service_register(service) == PXA_STATUS_OK);
    size = pxa_haptics_service_workspace_size(); void *hw = malloc(size); assert(hw);
    assert(pxa_haptics_service_init(hw, size, runtime, permissions, &motor, &haptics) == PXA_STATUS_OK);
    assert(pxa_haptics_service_register(haptics) == PXA_STATUS_OK);
    assert(call(component, PXA_PERIPHERALS_SERVICE_ID, 2, NULL, 0) == PXA_STATUS_BAD_STATE);
    const uint8_t identity[] = PXA_PERIPHERALS_IDENTITY; uint8_t hello[24] = {0}, packet[32] = {0};
    memcpy(hello, identity, 16); pxa_write_u16(hello+18, 1); pxa_write_u16(hello+22, 1);
    hello[0] ^= 1;
    assert(call(component, PXA_PERIPHERALS_SERVICE_ID, 1, hello, 24) == PXA_STATUS_UNSUPPORTED);
    hello[0] ^= 1;
    assert(call(component, PXA_PERIPHERALS_SERVICE_ID, 1, hello, 24) == PXA_STATUS_OK);
    assert(pxa_read_u16(reply.payload.data + 22) == 1);
    // A wider offered version range chooses the highest supported minor.
    pxa_write_u16(hello+20, 1); pxa_write_u16(hello+22, 0);
    assert(call(component, PXA_PERIPHERALS_SERVICE_ID, 1, hello, 24) == PXA_STATUS_OK);
    assert(pxa_read_u16(reply.payload.data + 22) == PXA_PERIPHERALS_MINOR);
    pxa_write_u16(hello+16, 1);
    assert(call(component, PXA_PERIPHERALS_SERVICE_ID, 1, hello, 24) == PXA_STATUS_UNSUPPORTED);
    pxa_write_u16(hello+16, 0); pxa_write_u16(hello+20, 0); pxa_write_u16(hello+22, 1);
    assert(call(component, PXA_PERIPHERALS_SERVICE_ID, 2, NULL, 0) == PXA_STATUS_OK);
    listed_kind = PXA_PERIPHERALS_RGB;
    assert(call(component, PXA_PERIPHERALS_SERVICE_ID, 1, hello, 24) == PXA_STATUS_OK);
    assert(pxa_read_u16(reply.payload.data + 22) == 1);
    assert(call(component, PXA_PERIPHERALS_SERVICE_ID, 2, NULL, 0) == PXA_STATUS_OK);
    assert(pxa_read_u16(reply.payload.data + 4) == 0); // RGB stays invisible to 0.1 clients.
    pxa_peripherals_service_set_foreground(service, component);
    pxa_write_u16(packet, 3); pxa_write_u16(packet + 2, PXA_PERIPHERALS_MODE_RGB);
    pxa_write_u64(packet + 12, UINT64_C(0x100000001));
    assert(call(component, PXA_PERIPHERALS_SERVICE_ID, 4, packet, 20) == PXA_STATUS_UNSUPPORTED);
    pxa_peripherals_service_set_foreground(service, 0);
    memset(packet, 0, sizeof(packet));
    pxa_write_u16(hello + 22, PXA_PERIPHERALS_MINOR);
    assert(call(component, PXA_PERIPHERALS_SERVICE_ID, 1, hello, 24) == PXA_STATUS_OK);
    assert(call(component, PXA_PERIPHERALS_SERVICE_ID, 2, NULL, 0) == PXA_STATUS_OK);
    assert(pxa_read_u16(reply.payload.data + 8) == PXA_PERIPHERALS_RGB);
    listed_kind = PXA_PERIPHERALS_VISION;
    pxa_write_u16(hello + 22, 2);
    assert(call(component, PXA_PERIPHERALS_SERVICE_ID, 1, hello, 24) == PXA_STATUS_OK);
    assert(call(component, PXA_PERIPHERALS_SERVICE_ID, 2, NULL, 0) == PXA_STATUS_OK);
    assert(pxa_read_u16(reply.payload.data + 4) == 0);
    pxa_peripherals_service_set_foreground(service, component);
    pxa_write_u16(packet, 3); pxa_write_u16(packet + 2, PXA_PERIPHERALS_MODE_STREAM);
    pxa_write_u64(packet + 12, UINT64_C(0x100000001));
    assert(call(component, PXA_PERIPHERALS_SERVICE_ID, 4, packet, 20) == PXA_STATUS_UNSUPPORTED);
    pxa_peripherals_service_set_foreground(service, 0);
    memset(packet, 0, sizeof(packet));
    pxa_write_u16(hello + 22, PXA_PERIPHERALS_MINOR);
    assert(call(component, PXA_PERIPHERALS_SERVICE_ID, 1, hello, 24) == PXA_STATUS_OK);
    assert(call(component, PXA_PERIPHERALS_SERVICE_ID, 2, NULL, 0) == PXA_STATUS_OK);
    assert(pxa_read_u16(reply.payload.data + 8) == PXA_PERIPHERALS_VISION);
    listed_kind = PXA_PERIPHERALS_I2C;
    const pxa_handle64_t permission = acquire_named(component, PXA_PERIPHERALS_PERMISSION, "expansion.i2c");
    pxa_write_u16(packet, 3); pxa_write_u16(packet+2, 3); pxa_write_u32(packet+4, 100000);
    pxa_write_u16(packet+8, 0x20); pxa_write_u64(packet+12, permission);
    assert(call(component, PXA_PERIPHERALS_SERVICE_ID, 4, packet, 20) == PXA_STATUS_DENIED);
    pxa_peripherals_service_set_foreground(service, component);
    assert(call(component, PXA_PERIPHERALS_SERVICE_ID, 4, packet, 20) == PXA_STATUS_OK);
    const pxa_handle64_t handle = pxa_read_u64(reply.payload.data+4);
    pxa_write_u64(packet, handle); pxa_write_u16(packet+8, 1); pxa_write_u16(packet+10, 2); packet[12] = 42;
    assert(dispatch(component, PXA_PERIPHERALS_SERVICE_ID, 5, packet, 13) == PXA_STATUS_OK);
    const uint64_t first = cookie;
    assert(starts == 1 && pxa_peripherals_service_pending(service));
    pxa_peripherals_service_poll(service); pxa_event_view_t event;
    assert(pxa_event_peek(runtime, component, &event) == PXA_STATUS_WOULD_BLOCK);
    ready = 1; pxa_peripherals_service_poll(service); pop(component);
    assert(reply.payload.size == 6 && reply.payload.data[4] == 10 && !pxa_peripherals_service_pending(service));
    // A backend cannot report a short/oversized success to Guest or leak bytes.
    const size_t malformed_lengths[]={0,1,3,257};
    for (unsigned i=0; i<sizeof(malformed_lengths)/sizeof(malformed_lengths[0]); ++i) {
        backend_bytes=malformed_lengths[i];
        assert(dispatch(component, PXA_PERIPHERALS_SERVICE_ID, 5, packet, 13) == PXA_STATUS_OK);
        ready=1; pxa_peripherals_service_poll(service); pop(component);
        assert(reply.payload.size==4 && pxa_read_u32(reply.payload.data)==(uint32_t)PXA_STATUS_PROTOCOL_ERROR);
    }
    backend_bytes=2; backend_poll_status=(pxa_status_t)-12345;
    assert(dispatch(component, PXA_PERIPHERALS_SERVICE_ID, 5, packet, 13) == PXA_STATUS_OK);
    ready=1; pxa_peripherals_service_poll(service); pop(component);
    assert(reply.payload.size==4 && pxa_read_u32(reply.payload.data)==(uint32_t)PXA_STATUS_IO_ERROR);
    backend_poll_status=PXA_STATUS_OK;
    // Once I/O is accepted it is committed: cancellation cannot reuse its
    // token until delivery, and a replacement gets a fresh native cookie.
    assert(dispatch(component, PXA_PERIPHERALS_SERVICE_ID, 5, packet, 13) == PXA_STATUS_OK);
    const pxa_request_token_t reused = token-1;
    assert(pxa_request_cancel(runtime, component, reused) == PXA_STATUS_OK);
    token = reused;
    assert(dispatch(component, PXA_PERIPHERALS_SERVICE_ID, 5, packet, 13) != PXA_STATUS_OK);
    ready = 1; pxa_peripherals_service_poll(service); pop(component);
    assert(reply.request_token == reused && pxa_read_u32(reply.payload.data) == 0);
    token = reused;
    assert(dispatch(component, PXA_PERIPHERALS_SERVICE_ID, 5, packet, 13) == PXA_STATUS_OK && cookie != first);
    pxa_peripherals_service_set_foreground(service, other);
    pxa_peripherals_service_poll(service); pop(component);
    assert(pxa_read_u32(reply.payload.data) == (uint32_t)PXA_STATUS_CANCELLED && closes == 1 && cancels == 1);
    assert(call(component, PXA_HAPTICS_SERVICE_ID, 1, NULL, 0) == PXA_STATUS_OK);
    assert(pxa_read_u32(reply.payload.data+4) == 1 && pxa_read_u32(reply.payload.data+8) == 1000);
    const pxa_handle64_t hperm = acquire_named(component, PXA_HAPTICS_PERMISSION, PXA_HAPTICS_SCOPE);
    pxa_write_u64(packet, hperm);
    assert(call(component, 23, 2, packet, 8) == PXA_STATUS_DENIED);
    pxa_haptics_service_set_foreground(haptics, component);
    assert(call(component, 23, 2, packet, 8) == PXA_STATUS_OK);
    const pxa_handle64_t actuator = pxa_read_u64(reply.payload.data+4);
    pxa_write_u64(packet, actuator); pxa_write_u32(packet+8, 100); pxa_write_u16(packet+12, 500); pxa_write_u16(packet+14, 0);
    assert(call(component, 23, 3, packet, 16) == PXA_STATUS_UNSUPPORTED && !plays);
    pxa_write_u16(packet+12, 1000);
    assert(call(other, 23, 3, packet, 16) != PXA_STATUS_OK && !plays);
    assert(call(component, 23, 3, packet, 16) == PXA_STATUS_OK && plays == 1);
    pxa_write_u32(packet+8, 1001);
    assert(call(component, 23, 3, packet, 16) == PXA_STATUS_INVALID_ARGUMENT && plays == 1);
    pxa_haptics_service_set_foreground(haptics, other);
    assert(stops == 1 && call(component, 23, 4, packet, 8) == PXA_STATUS_NOT_FOUND);
    pxa_haptics_service_set_foreground(haptics, component); pxa_write_u64(packet, hperm);
    assert(call(component, 23, 2, packet, 8) == PXA_STATUS_OK);
    assert(pxa_handle_close64(runtime, component, hperm) == PXA_STATUS_OK);
    assert(stops == 2);
    pxa_peripherals_service_set_foreground(service, component);
    memset(packet, 0, sizeof(packet));
    pxa_write_u16(packet, 3); pxa_write_u16(packet+2, 3); pxa_write_u32(packet+4, 100000);
    pxa_write_u16(packet+8, 0x20); pxa_write_u64(packet+12, permission);
    const unsigned initial_closes=closes;
    backend_open_status=PXA_STATUS_IO_ERROR;
    assert(call(component, PXA_PERIPHERALS_SERVICE_ID, 4, packet, 20)==PXA_STATUS_IO_ERROR);
    assert(reply.payload.size==4 && closes==initial_closes+1);
    backend_open_status=(pxa_status_t)-12345;
    assert(call(component, PXA_PERIPHERALS_SERVICE_ID, 4, packet, 20)==PXA_STATUS_IO_ERROR);
    assert(closes==initial_closes+2);
    backend_null_open=1;
    assert(call(component, PXA_PERIPHERALS_SERVICE_ID, 4, packet, 20)==PXA_STATUS_IO_ERROR);
    backend_open_status=PXA_STATUS_OK;
    assert(call(component, PXA_PERIPHERALS_SERVICE_ID, 4, packet, 20)==PXA_STATUS_PROTOCOL_ERROR);
    assert(closes==initial_closes+2);
    backend_null_open=0;
    assert(call(component, PXA_PERIPHERALS_SERVICE_ID, 4, packet, 20)==PXA_STATUS_OK);
    const pxa_handle64_t revoked_handle=pxa_read_u64(reply.payload.data+4);
    pxa_write_u64(packet, revoked_handle); pxa_write_u16(packet+8, 1); pxa_write_u16(packet+10, 2); packet[12]=42;
    assert(dispatch(component, PXA_PERIPHERALS_SERVICE_ID, 5, packet, 13)==PXA_STATUS_OK);
    const unsigned before_revoke=cancels;
    assert(pxa_handle_close64(runtime, component, permission)==PXA_STATUS_OK);
    assert(!pxa_peripherals_service_pending(service) && cancels==before_revoke+1 && closes==initial_closes+3);
    pop(component); assert(reply.payload.size==4 && pxa_read_u32(reply.payload.data)==(uint32_t)PXA_STATUS_CANCELLED);
    const unsigned before_stale=starts;
    assert(call(component, PXA_PERIPHERALS_SERVICE_ID, 5, packet, 13)==PXA_STATUS_NOT_FOUND && starts==before_stale);
    const pxa_handle64_t fresh_permission=acquire_named(component, PXA_PERIPHERALS_PERMISSION, "expansion.i2c");
    memset(packet, 0, sizeof(packet));
    pxa_write_u16(packet, 3); pxa_write_u16(packet+2, 3); pxa_write_u32(packet+4, 100000);
    pxa_write_u16(packet+8, 0x20); pxa_write_u64(packet+12, fresh_permission);
    assert(call(component, PXA_PERIPHERALS_SERVICE_ID, 4, packet, 20)==PXA_STATUS_OK);
    const pxa_handle64_t stopped_handle=pxa_read_u64(reply.payload.data+4);
    pxa_write_u64(packet, stopped_handle); pxa_write_u16(packet+8, 1); pxa_write_u16(packet+10, 2); packet[12]=42;
    assert(dispatch(component, PXA_PERIPHERALS_SERVICE_ID, 5, packet, 13)==PXA_STATUS_OK);
    const unsigned before_stop=cancels;
    assert(pxa_component_request_stop(runtime, component, PXA_STOP_NORMAL)==PXA_STATUS_OK);
    assert(pxa_component_begin_stop(runtime, component)==PXA_STATUS_OK);
    assert(pxa_component_finish_stop(runtime, component)==PXA_STATUS_OK);
    assert(!pxa_peripherals_service_pending(service) && cancels==before_stop+1 && closes==initial_closes+4);
    pxa_peripherals_service_poll(service); // No stale backend pointers survive component teardown.
    const pxa_component_t previous_component=component;
    assert(pxa_component_remove(runtime, component)==PXA_STATUS_OK);
    component=create(3); assert(component!=previous_component);
    assert(call(component, PXA_PERIPHERALS_SERVICE_ID, 2, NULL, 0)==PXA_STATUS_BAD_STATE);
    assert(call(component, PXA_PERIPHERALS_SERVICE_ID, 1, hello, sizeof(hello))==PXA_STATUS_OK);
    assert(call(component, PXA_PERIPHERALS_SERVICE_ID, 2, NULL, 0)==PXA_STATUS_OK);
    assert(call(component, PXA_PERIPHERALS_SERVICE_ID, 5, packet, 13)==PXA_STATUS_NOT_FOUND);
    const pxa_handle64_t restarted_permission=acquire_named(component, PXA_PERIPHERALS_PERMISSION, "expansion.i2c");
    pxa_peripherals_service_set_foreground(service, component);
    memset(packet, 0, sizeof(packet));
    pxa_write_u16(packet, 3); pxa_write_u16(packet+2, 3); pxa_write_u32(packet+4, 100000);
    pxa_write_u16(packet+8, 0x20); pxa_write_u64(packet+12, restarted_permission);
    assert(call(component, PXA_PERIPHERALS_SERVICE_ID, 4, packet, 20)==PXA_STATUS_OK);
    const pxa_handle64_t restarted_handle=pxa_read_u64(reply.payload.data+4);
    assert(restarted_handle!=stopped_handle);
    pxa_write_u64(packet, restarted_handle); pxa_write_u16(packet+8, 1); pxa_write_u16(packet+10, 2); packet[12]=42;
    assert(dispatch(component, PXA_PERIPHERALS_SERVICE_ID, 5, packet, 13)==PXA_STATUS_OK);
    ready=1; pxa_peripherals_service_poll(service); pop(component);
    assert(reply.payload.size==6 && pxa_read_u32(reply.payload.data)==0);

    // Finite JPEG streams preserve camera authority and frame boundaries.
    camera_mode = 1;
    pxa_write_u16(packet, 3);
    assert(call(component, PXA_PERIPHERALS_SERVICE_ID, 7, packet, 2) == PXA_STATUS_OK);
    assert(reply.payload.size == 54 && pxa_read_u16(reply.payload.data+4) == 2);
    assert(pxa_read_u32(reply.payload.data+6) == 1 && pxa_read_u16(reply.payload.data+12) == 320);
    invalid_profile = 1;
    assert(call(component, PXA_PERIPHERALS_SERVICE_ID, 7, packet, 2) == PXA_STATUS_PROTOCOL_ERROR);
    invalid_profile = 0;
    hd_profile = 1;
    assert(call(component, PXA_PERIPHERALS_SERVICE_ID, 7, packet, 2) == PXA_STATUS_OK);
    assert(pxa_read_u16(reply.payload.data+4) == 2 && pxa_read_u32(reply.payload.data+42) == PXA_VISION_MAX_JPEG_HD);
    pxa_write_u16(hello+22, 7);
    assert(call(component, PXA_PERIPHERALS_SERVICE_ID, 1, hello, 24) == PXA_STATUS_OK);
    assert(call(component, PXA_PERIPHERALS_SERVICE_ID, 7, packet, 2) == PXA_STATUS_OK);
    assert(pxa_read_u16(reply.payload.data+4) == 1 && reply.payload.size == 30);
    assert(pxa_read_u32(reply.payload.data+26) == PXA_CAMERA_PROFILE_DEFAULT);
    hd_profile = 0;
    pxa_write_u16(hello+22, 4);
    assert(call(component, PXA_PERIPHERALS_SERVICE_ID, 1, hello, 24) == PXA_STATUS_OK);
    assert(call(component, PXA_PERIPHERALS_SERVICE_ID, 7, packet, 2) == PXA_STATUS_UNSUPPORTED);
    pxa_write_u16(hello+22, 5);
    assert(call(component, PXA_PERIPHERALS_SERVICE_ID, 1, hello, 24) == PXA_STATUS_OK);
    pxa_vision_frame_info_t fi = {PXA_VISION_JPEG, 8, 1, 1234};
    assert(pxa_vision_encode(frame_bytes, &fi));
    memcpy(frame_bytes + PXA_VISION_HEADER_BYTES, "JPEGdata", 8);
    const pxa_handle64_t camera_permission = acquire_named(component, PXA_PERIPHERALS_PERMISSION, "camera.jpeg");
    memset(packet, 0, sizeof(packet));
    pxa_write_u16(packet, 3); pxa_write_u16(packet+2, 7); pxa_write_u64(packet+12, camera_permission);
    assert(call(component, PXA_PERIPHERALS_SERVICE_ID, 4, packet, 20) == PXA_STATUS_OK);
    pxa_handle64_t camera_handle = pxa_read_u64(reply.payload.data+4);
    pxa_write_u64(packet, camera_handle);
    assert(call(component,PXA_PERIPHERALS_SERVICE_ID,9,packet,8)==PXA_STATUS_UNSUPPORTED);
    pxa_write_u16(hello+22,7);
    assert(!call(component,PXA_PERIPHERALS_SERVICE_ID,1,hello,24));
    assert(call(component,PXA_PERIPHERALS_SERVICE_ID,9,packet,7)==PXA_STATUS_INVALID_ARGUMENT);
    preview_block=2;
    assert(!dispatch(component,PXA_PERIPHERALS_SERVICE_ID,9,packet,8));
    const pxa_request_token_t preview_token=token-1;
    assert(pxa_peripherals_service_pending(service) && !image_allocations);
    assert(call(component,PXA_PERIPHERALS_SERVICE_ID,9,packet,8)==PXA_STATUS_BUSY);
    assert(call(component,PXA_PERIPHERALS_SERVICE_ID,6,packet,8)==PXA_STATUS_BUSY);
    uint8_t busy_byte=0;
    assert(read_resource(component,camera_handle,&busy_byte,1)==PXA_STATUS_BUSY);
    pxa_peripherals_service_poll(service);
    assert(pxa_peripherals_service_pending(service) && !image_allocations);
    pxa_peripherals_service_poll(service); pop(component);
    assert(reply.request_token==preview_token && !pxa_read_u32(reply.payload.data));
    assert(!pxa_peripherals_service_pending(service));
    const pxa_handle64_t preview_handle=pxa_read_u64(reply.payload.data+4);
    assert(reply.payload.size==32 && image_allocations==1);
    pxa_raster_asset_t* retained=NULL;
    assert(pxa_assets_acquire_handle(runtime,other,preview_handle,PXA_ASSET_IMAGE,&retained)!=PXA_STATUS_OK);
    assert(!pxa_assets_acquire_handle(runtime,component,preview_handle,PXA_ASSET_IMAGE,&retained));
    assert(!pxa_handle_close64(runtime,component,preview_handle) && image_allocations==1);
    pxa_raster_asset_release(retained); assert(!image_allocations);
    preview_width=840;
    assert(call(component,PXA_PERIPHERALS_SERVICE_ID,9,packet,8)==PXA_STATUS_UNSUPPORTED && !image_allocations);
    pxa_write_u16(hello+22,8);
    assert(!call(component,PXA_PERIPHERALS_SERVICE_ID,1,hello,24));
    assert(!call(component,PXA_PERIPHERALS_SERVICE_ID,9,packet,8));
    assert(pxa_read_u16(reply.payload.data+16)==840);
    assert(!pxa_handle_close64(runtime,component,pxa_read_u64(reply.payload.data+4)) && !image_allocations);
    preview_width=1025;
    assert(call(component,PXA_PERIPHERALS_SERVICE_ID,9,packet,8)==PXA_STATUS_PROTOCOL_ERROR && !image_allocations);
    preview_width=2;
    pxa_handle64_t held_images[2];
    for (unsigned i=0;i<2;++i) {
        assert(!call(component,PXA_PERIPHERALS_SERVICE_ID,9,packet,8)); held_images[i]=pxa_read_u64(reply.payload.data+4);
    }
    assert(call(component,PXA_PERIPHERALS_SERVICE_ID,9,packet,8)==PXA_STATUS_RESOURCE_LIMIT && image_allocations==2);
    for (unsigned i=0;i<2;++i) assert(!pxa_handle_close64(runtime,component,held_images[i]));
    assert(!image_allocations);
    preview_block=1;
    assert(!dispatch(component,PXA_PERIPHERALS_SERVICE_ID,9,packet,8));
    preview_error=PXA_STATUS_IO_ERROR;
    pxa_peripherals_service_poll(service); pop(component);
    assert(pxa_read_u32(reply.payload.data)==(uint32_t)PXA_STATUS_IO_ERROR && !image_allocations);
    preview_error=PXA_STATUS_OK;
    preview_block=1;
    assert(!dispatch(component,PXA_PERIPHERALS_SERVICE_ID,9,packet,8));
    assert(pxa_peripherals_service_pending(service));
    assert(!pxa_request_cancel(runtime,component,token-1));
    // Accepted peripheral commands are committed. Cancelling the waiter
    // still delivers its resource once, so the SDK can reclaim it.
    assert(pxa_request_identity(runtime,component,token-1));
    pxa_peripherals_service_poll(service);
    pop(component); assert(!pxa_read_u32(reply.payload.data));
    assert(!pxa_handle_close64(runtime,component,pxa_read_u64(reply.payload.data+4)));
    preview_block=1;
    assert(!dispatch(component,PXA_PERIPHERALS_SERVICE_ID,9,packet,8));
    assert(!pxa_handle_close64(runtime,component,camera_handle));
    pop(component);
    assert(pxa_read_u32(reply.payload.data)==(uint32_t)PXA_STATUS_CANCELLED);
    pxa_resource_t cancelled_camera;
    assert(pxa_handle_get64(runtime,component,camera_handle,PXA_PERIPHERALS_RESOURCE_TYPE,&cancelled_camera)!=PXA_STATUS_OK);
    assert(!image_allocations && !pxa_peripherals_service_pending(service));
    memset(packet,0,20); pxa_write_u16(packet,3); pxa_write_u16(packet+2,7); pxa_write_u64(packet+12,camera_permission);
    assert(!call(component,PXA_PERIPHERALS_SERVICE_ID,4,packet,20));
    camera_handle=pxa_read_u64(reply.payload.data+4); pxa_write_u64(packet,camera_handle);
    preview_block=1;
    assert(!dispatch(component,PXA_PERIPHERALS_SERVICE_ID,9,packet,8));
    pxa_peripherals_service_set_foreground(service,other); pop(component);
    assert(pxa_read_u32(reply.payload.data)==(uint32_t)PXA_STATUS_CANCELLED);
    assert(!image_allocations && !pxa_peripherals_service_pending(service));
    pxa_peripherals_service_set_foreground(service,component);
    memset(packet,0,20); pxa_write_u16(packet,3); pxa_write_u16(packet+2,7); pxa_write_u64(packet+12,camera_permission);
    assert(!call(component,PXA_PERIPHERALS_SERVICE_ID,4,packet,20));
    camera_handle=pxa_read_u64(reply.payload.data+4); pxa_write_u64(packet,camera_handle);
    pxa_write_u16(hello + 22, 3);
    assert(call(component, PXA_PERIPHERALS_SERVICE_ID, 1, hello, 24) == PXA_STATUS_OK);
    assert(call(component, PXA_PERIPHERALS_SERVICE_ID, 6, packet, 8) == PXA_STATUS_UNSUPPORTED);
    pxa_write_u16(hello + 22, 4);
    assert(call(component, PXA_PERIPHERALS_SERVICE_ID, 1, hello, 24) == PXA_STATUS_OK);
    assert(call(other, PXA_PERIPHERALS_SERVICE_ID, 1, hello, 24) == PXA_STATUS_OK);
    assert(call(other, PXA_PERIPHERALS_SERVICE_ID, 6, packet, 8) == PXA_STATUS_DENIED);
    assert(call(component, PXA_PERIPHERALS_SERVICE_ID, 6, packet, 8) == PXA_STATUS_OK);
    pxa_handle64_t frame_handle = pxa_read_u64(reply.payload.data+4);
    assert(call(component, PXA_PERIPHERALS_SERVICE_ID, 6, packet, 8) == PXA_STATUS_BUSY);
    uint8_t data[64] = {0}; size_t received = 0;
    assert(read_resource(component, camera_handle, data, 8) == PXA_STATUS_BUSY);
    assert(read_resource(other, frame_handle, data, 8) < 0);
    frame_block = 1;
    for (unsigned i = 0; i < 20; ++i) {
        int32_t n = read_resource(component, frame_handle, data + received, sizeof(data) - received);
        if (n == PXA_STATUS_WOULD_BLOCK) continue;
        assert(n >= 0); if (!n) break; received += (size_t)n;
    }
    assert(received == 8 && !memcmp(data, "JPEGdata", 8));
    assert(read_resource(component, frame_handle, data, sizeof(data)) == 0);
    assert(pxa_handle_close64(runtime, component, frame_handle) == PXA_STATUS_OK);
    pxa_resource_t parent;
    assert(pxa_handle_get64(runtime, component, camera_handle, PXA_PERIPHERALS_RESOURCE_TYPE, &parent) == PXA_STATUS_OK);
    fi.size=5120; assert(pxa_vision_encode(frame_bytes,&fi));
    for (unsigned i=0;i<fi.size;++i) frame_bytes[PXA_VISION_HEADER_BYTES+i]=(uint8_t)i;
    frame_length=PXA_VISION_HEADER_BYTES+fi.size; frame_offset=0; frame_chunk=256; native_reads=0;
    assert(!call(component,PXA_PERIPHERALS_SERVICE_ID,6,packet,8));
    frame_handle=pxa_read_u64(reply.payload.data+4);
    uint8_t bulk[8192];
    assert(read_resource(component,frame_handle,bulk,sizeof(bulk))==4096 && native_reads==17);
    assert(read_resource(component,frame_handle,bulk+4096,sizeof(bulk)-4096)==1024 && native_reads==21);
    for (unsigned i=0;i<fi.size;++i) assert(bulk[i]==(uint8_t)i);
    assert(read_resource(component,frame_handle,bulk,sizeof(bulk))==0);
    assert(!pxa_handle_close64(runtime,component,frame_handle));
    // 0.8 batches more bounded native reads, preserving the old 4 KiB contract.
    pxa_write_u16(hello+22,8);
    assert(!call(component,PXA_PERIPHERALS_SERVICE_ID,1,hello,24));
    fi.size=24576; assert(pxa_vision_encode(frame_bytes,&fi));
    for (unsigned i=0;i<fi.size;++i) frame_bytes[PXA_VISION_HEADER_BYTES+i]=(uint8_t)i;
    frame_length=PXA_VISION_HEADER_BYTES+fi.size; frame_offset=0; native_reads=0;
    assert(!call(component,PXA_PERIPHERALS_SERVICE_ID,6,packet,8));
    frame_handle=pxa_read_u64(reply.payload.data+4);
    uint8_t hd_bulk[24576];
    assert(read_resource(component,frame_handle,hd_bulk,sizeof(hd_bulk))==16384 && native_reads==65);
    assert(read_resource(component,frame_handle,hd_bulk+16384,8192)==8192 && native_reads==97);
    for (unsigned i=0;i<fi.size;++i) assert(hd_bulk[i]==(uint8_t)i);
    assert(!pxa_handle_close64(runtime,component,frame_handle));
    pxa_write_u16(hello+22,4);
    assert(!call(component,PXA_PERIPHERALS_SERVICE_ID,1,hello,24));
    fi.size=5120; assert(pxa_vision_encode(frame_bytes,&fi)); frame_length=PXA_VISION_HEADER_BYTES+fi.size;
    frame_offset=0; frame_block_after=PXA_VISION_HEADER_BYTES+256;
    assert(!call(component,PXA_PERIPHERALS_SERVICE_ID,6,packet,8));
    frame_handle=pxa_read_u64(reply.payload.data+4);
    size_t bulk_received=0;
    assert(read_resource(component,frame_handle,bulk,sizeof(bulk))==256);
    bulk_received=256;
    while (bulk_received<fi.size) {
        const int32_t n=read_resource(component,frame_handle,bulk+bulk_received,sizeof(bulk)-bulk_received);
        assert(n>0); bulk_received+=(size_t)n;
    }
    for (unsigned i=0;i<fi.size;++i) assert(bulk[i]==(uint8_t)i);
    assert(!pxa_handle_close64(runtime,component,frame_handle));
    fi.size=64; assert(pxa_vision_encode(frame_bytes,&fi));
    frame_length=PXA_VISION_HEADER_BYTES+fi.size; frame_offset=0; frame_chunk=1;
    assert(!call(component,PXA_PERIPHERALS_SERVICE_ID,6,packet,8));
    frame_handle=pxa_read_u64(reply.payload.data+4); bulk_received=0;
    while (bulk_received<fi.size) {
        const size_t calls_before=native_reads;
        const int32_t n=read_resource(component,frame_handle,bulk+bulk_received,sizeof(bulk)-bulk_received);
        assert(native_reads-calls_before<=17);
        assert(n>0 || n==PXA_STATUS_WOULD_BLOCK);
        if (n>0) bulk_received+=(size_t)n;
    }
    assert(!pxa_handle_close64(runtime,component,frame_handle));
    fi.size=8; assert(pxa_vision_encode(frame_bytes,&fi));
    memcpy(frame_bytes+PXA_VISION_HEADER_BYTES,"JPEGdata",8);
    frame_length=32; frame_chunk=7;
    // The next frame starts after the prior payload, without reopening camera.
    frame_offset = 0;
    assert(call(component, PXA_PERIPHERALS_SERVICE_ID, 6, packet, 8) == PXA_STATUS_OK);
    frame_handle = pxa_read_u64(reply.payload.data+4);
    assert(read_resource(component, frame_handle, data, sizeof(data)) == PXA_STATUS_WOULD_BLOCK);
    assert(pxa_handle_close64(runtime, component, frame_handle) == PXA_STATUS_OK);
    assert(pxa_handle_get64(runtime, component, camera_handle, PXA_PERIPHERALS_RESOURCE_TYPE, &parent) == PXA_STATUS_NOT_FOUND);
    // Parent-slot reuse cannot reconnect an old stream to a new camera.
    memset(packet, 0, sizeof(packet));
    pxa_write_u16(packet, 3); pxa_write_u16(packet+2, 7); pxa_write_u64(packet+12, camera_permission);
    assert(call(component, PXA_PERIPHERALS_SERVICE_ID, 4, packet, 20) == PXA_STATUS_OK);
    camera_handle = pxa_read_u64(reply.payload.data+4); pxa_write_u64(packet, camera_handle);
    assert(call(component, PXA_PERIPHERALS_SERVICE_ID, 6, packet, 8) == PXA_STATUS_OK);
    frame_handle = pxa_read_u64(reply.payload.data+4);
    frame_offset = 0; frame_chunk = 32; frame_bytes[0] = '!';
    assert(read_resource(component, frame_handle, data, sizeof(data)) == PXA_STATUS_PROTOCOL_ERROR);
    assert(read_resource(component, frame_handle, data, sizeof(data)) == PXA_STATUS_PROTOCOL_ERROR);
    assert(pxa_handle_close64(runtime, component, camera_permission) == PXA_STATUS_OK);
    assert(read_resource(component, frame_handle, data, sizeof(data)) == PXA_STATUS_NOT_FOUND);
    const pxa_handle64_t fresh_camera_permission = acquire_named(component, PXA_PERIPHERALS_PERMISSION, "camera.jpeg");
    memset(packet, 0, sizeof(packet));
    pxa_write_u16(packet, 3); pxa_write_u16(packet+2, 7); pxa_write_u64(packet+12, fresh_camera_permission);
    assert(call(component, PXA_PERIPHERALS_SERVICE_ID, 4, packet, 20) == PXA_STATUS_OK);
    camera_handle = pxa_read_u64(reply.payload.data+4); pxa_write_u64(packet, camera_handle);
    assert(call(component, PXA_PERIPHERALS_SERVICE_ID, 6, packet, 8) == PXA_STATUS_OK);
    frame_handle = pxa_read_u64(reply.payload.data+4);
    pxa_peripherals_service_set_foreground(service, other);
    assert(read_resource(component, frame_handle, data, sizeof(data)) == PXA_STATUS_NOT_FOUND);
    assert(pxa_handle_get64(runtime, component, camera_handle, PXA_PERIPHERALS_RESOURCE_TYPE, &parent) == PXA_STATUS_NOT_FOUND);

    pxa_peripherals_service_set_foreground(service, component);
    pxa_write_u16(hello+22, 6);
    assert(call(component, PXA_PERIPHERALS_SERVICE_ID, 1, hello, 24) == PXA_STATUS_OK);
    pxa_resource_t file_resource={NULL,&file_ops,file_close}; pxa_handle64_t file_handle;
    assert(pxa_handle_open64(runtime,component,PXA_RESOURCE_FILE,0,&file_resource,&file_handle)==PXA_STATUS_OK);
    pxa_write_u64(packet,file_handle);pxa_write_u64(packet+8,2);pxa_write_u32(packet+16,8);
    pxa_write_u64(packet+20,0);
    assert(call(component,PXA_PERIPHERALS_SERVICE_ID,8,packet,28)==PXA_STATUS_INVALID_ARGUMENT);
    pxa_write_u64(packet+20, UINT64_MAX-7);
    assert(call(component,PXA_PERIPHERALS_SERVICE_ID,8,packet,28)==PXA_STATUS_INVALID_ARGUMENT);
    pxa_write_u64(packet+20,100);
    pxa_write_u64(packet+20, pxa_read_u64(packet+20)+8);
    assert(call(component,PXA_PERIPHERALS_SERVICE_ID,8,packet,28)==PXA_STATUS_OK);
    frame_handle=pxa_read_u64(reply.payload.data+4);file_block=1;
    assert(read_resource(component,frame_handle,data,8)==PXA_STATUS_WOULD_BLOCK);
    assert(read_resource(component,frame_handle,data,3)==3 && !memcmp(data,"234",3));
    assert(read_resource(component,frame_handle,data,8)==5 && !memcmp(data,"56789",5));
    assert(read_resource(component,frame_handle,data,8)==0);
    assert(pxa_handle_close64(runtime,component,frame_handle)==PXA_STATUS_OK && !file_closed);
    // A caller's metadata ReadAt between JPEG streams uses the next range.
    uint8_t metadata[25]={0};
    pxa_write_u64(metadata, 0); pxa_write_u64(metadata+8, 115); pxa_write_u32(metadata+16, 1);
    assert(pxa_component_begin_event(runtime, component) == PXA_STATUS_OK);
    assert(pxa_runtime_io64(runtime, component, file_handle, PXA_FS_IO_READ_AT, metadata, sizeof(metadata)) == 1);
    assert(pxa_component_finish_event(runtime, component, 1) == PXA_STATUS_OK);
    pxa_write_u64(packet+20, pxa_read_u64(packet+20)+8);
    assert(call(component,PXA_PERIPHERALS_SERVICE_ID,8,packet,28)==PXA_STATUS_OK);
    frame_handle=pxa_read_u64(reply.payload.data+4);file_block=1;
    assert(read_resource(component,frame_handle,data,8)==PXA_STATUS_WOULD_BLOCK);
    assert(pxa_handle_close64(runtime,component,frame_handle)==PXA_STATUS_OK && file_cancelled==1 && !file_closed);
    file_cookie=0;
    pxa_write_u64(packet+20, pxa_read_u64(packet+20)+8);
    assert(call(component,PXA_PERIPHERALS_SERVICE_ID,8,packet,28)==PXA_STATUS_OK);
    frame_handle=pxa_read_u64(reply.payload.data+4);
    assert(read_resource(component,frame_handle,data,1)==1);
    assert(pxa_handle_close64(runtime,component,file_handle)==PXA_STATUS_OK && file_closed==1);
    assert(read_resource(component,frame_handle,data,8)<0); // cached bytes respect revoked parent lifetime
    assert(pxa_handle_close64(runtime,component,frame_handle)==PXA_STATUS_OK);
    assert(call(other,PXA_PERIPHERALS_SERVICE_ID,8,packet,28)!=PXA_STATUS_OK);

    pxa_runtime_deinit(runtime); free(aw); free(hw); free(sw); free(pw); free(workspace);
    puts("Peripherals and Haptics: negotiation, authorization, async I/O, isolation, cancellation, revocation, partial-open cleanup and component teardown passed");
}
