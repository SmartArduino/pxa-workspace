// SPDX-License-Identifier: Apache-2.0
#include <pxa_ext/peripherals.h>
#include <pxa/haptics.h>
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
static uint16_t listed_kind = PXA_PERIPHERALS_I2C;
static pxa_status_t port_list(void *ctx, pxa_peripheral_port_t *p, size_t capacity, size_t *count) {
    (void)ctx; assert(capacity >= 1); *count = 1;
    *p = (pxa_peripheral_port_t){3, listed_kind, 3, "expansion.i2c"}; return PXA_STATUS_OK;
}
static pxa_status_t module(void *ctx, uint16_t port, pxa_peripheral_module_t *out) {
    (void)ctx; if (port != 1) return PXA_STATUS_NOT_FOUND;
    *out = (pxa_peripheral_module_t){1, 3, 1, 42, 0x100, 1, 100, "test"}; return PXA_STATUS_OK;
}
static pxa_status_t port_open(void *ctx, uint64_t owner, uint16_t port, uint16_t mode,
    uint32_t parameter, uint16_t address, void **out) {
    (void)ctx; assert(owner >> 32); assert(port == 3 && mode == 3 && parameter == 100000 && address == 0x20);
    ++opens; if (!backend_null_open) *out = &opens; return backend_open_status;
}
static int32_t io(void *ctx, void *device, uint32_t op, uint8_t *bytes, size_t size) {
    (void)ctx; (void)device; (void)op; (void)bytes; (void)size; return PXA_STATUS_UNSUPPORTED;
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
         {(const uint8_t *)PXA_HAPTICS_SCOPE, sizeof(PXA_HAPTICS_SCOPE)-1}, 0}};
    const pxa_peripherals_backend_t backend = {sizeof(backend), NULL, port_list, module,
        port_open, io, start, poll, cancel, close_port, NULL};
    const pxa_haptics_backend_t motor = {sizeof(motor), NULL, 1, 1000, play, stop};
    pxa_runtime_limits_init(&limits); limits.max_services = 4; limits.max_components = 2;
    limits.max_requests = 8; limits.max_requests_per_component = 4;
    limits.max_handles = 12; limits.max_events = 10; limits.mailbox_capacity = 6;
    limits.event_block_count = 128; limits.event_block_size = 64;
    size = pxa_runtime_workspace_size(&limits); void *workspace = malloc(size); assert(workspace);
    assert(pxa_runtime_init(workspace, size, &limits, &runtime) == PXA_STATUS_OK);
    component = create(1); other = create(2);
    config.struct_size = sizeof(config); config.app_identity = (pxa_bytes_t){(const uint8_t *)"test", 4};
    config.declarations = declarations; config.declaration_count = 2; config.max_authorities = 2;
    config.store = (pxa_permission_store_t){sizeof(config.store), NULL, load, save};
    size = pxa_permission_service_workspace_size(&config); void *pw = malloc(size); assert(pw);
    assert(pxa_permission_service_init(pw, size, runtime, &config, &permissions) == PXA_STATUS_OK);
    assert(pxa_permission_policy_load(permissions) == PXA_STATUS_OK);
    assert(pxa_permission_service_register(permissions) == PXA_STATUS_OK);
    size = pxa_peripherals_service_workspace_size(); void *sw = malloc(size); assert(sw);
    assert(pxa_peripherals_service_init(sw, size, runtime, permissions, &backend, &service) == PXA_STATUS_OK);
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

    pxa_runtime_deinit(runtime); free(hw); free(sw); free(pw); free(workspace);
    puts("Peripherals and Haptics: negotiation, authorization, async I/O, isolation, cancellation, revocation, partial-open cleanup and component teardown passed");
}
