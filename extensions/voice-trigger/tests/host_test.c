// SPDX-License-Identifier: Apache-2.0
#include <pxa_ext/voice_trigger.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pxa_runtime_t *runtime;
static pxa_component_t component, other;
static pxa_vt_service_t *service;
static pxa_permission_service_t *permissions;
static pxa_request_token_t token = 1;
static uint8_t response[2048];
static pxa_message_view_t reply;
static struct {
    pxa_vt_profile_t profile;
    pxa_vt_match_t queue[8];
    unsigned head, count, starts, stops, reads;
    pxa_status_t error;
    int enabled;
} backend;

static pxa_status_t profile(void *ctx, pxa_vt_profile_t *output) {
    (void)ctx; *output = backend.profile; return PXA_STATUS_OK;
}
static pxa_status_t start(void *ctx, uint32_t revision) {
    (void)ctx;
    assert(revision == backend.profile.revision && !backend.enabled);
    backend.enabled = 1; ++backend.starts;
    backend.head = backend.count = 0;
    return PXA_STATUS_OK;
}
static void stop(void *ctx) {
    (void)ctx;
    assert(backend.enabled);
    backend.enabled = 0; ++backend.stops;
    backend.count = 0;
}
static pxa_status_t read_match(void *ctx, pxa_vt_match_t *out) {
    (void)ctx; ++backend.reads;
    assert(backend.enabled);
    if (backend.error) return backend.error;
    if (!backend.count) return PXA_STATUS_WOULD_BLOCK;
    *out = backend.queue[backend.head++]; --backend.count;
    return PXA_STATUS_OK;
}
static void push(uint64_t seq, uint64_t time, uint32_t keyword) {
    assert(backend.head + backend.count < 8);
    backend.queue[backend.head + backend.count++] =
        (pxa_vt_match_t){seq, time, 7, keyword};
}
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
static pxa_handle64_t acquire(pxa_component_t c) {
    return acquire_named(c, PXA_VT_PERMISSION, PXA_VT_DEFAULT_SCOPE);
}
static pxa_status_t subscribe(pxa_component_t c, pxa_handle64_t permission,
                              uint32_t revision, pxa_handle64_t *handle) {
    uint8_t payload[16];
    pxa_status_t status;
    pxa_write_u32(payload, PXA_VT_DEFAULT_PROFILE);
    pxa_write_u32(payload + 4, revision);
    pxa_write_u64(payload + 8, permission);
    status = call(c, PXA_VT_SERVICE_ID, PXA_VT_SUBSCRIBE, payload, sizeof(payload));
    if (status == PXA_STATUS_OK && handle) {
        assert(reply.payload.size == 16 && pxa_read_u32(reply.payload.data + 12) == 7);
        *handle = pxa_read_u64(reply.payload.data + 4);
    }
    return status;
}
static void empty(void) {
    pxa_event_view_t view;
    assert(pxa_event_peek(runtime, component, &view) == PXA_STATUS_WOULD_BLOCK);
}
int main(void) {
    static const uint8_t identity[] = PXA_VT_IDENTITY;
    const pxa_vt_backend_t ops = {sizeof(ops), NULL, profile, start, read_match, stop};
    pxa_runtime_limits_t limits;
    pxa_permission_config_t config = {0};
#define DECLARE(name, scope) {{(const uint8_t *)name, sizeof(name)-1}, \
    {(const uint8_t *)scope, sizeof(scope)-1}, 0}
    const pxa_permission_declaration_t declarations[] = {
        DECLARE("audio.capture", "microphone"),
        DECLARE("audio.playback", "media"),
        DECLARE("net.client", "web"),
        DECLARE("net.socket", "outbound"),
        DECLARE(PXA_VT_PERMISSION, PXA_VT_DEFAULT_SCOPE)};
#undef DECLARE
    void *workspace, *permission_workspace, *service_workspace;
    size_t size;
    pxa_handle64_t permission, handle, previous;
    uint8_t hello[24], payload[8];
    pxa_resource_t resource;
    pxa_runtime_limits_init(&limits);
    limits.max_components = 2; limits.max_requests = 4;
    limits.max_requests_per_component = 4; limits.max_handles = 8;
    limits.max_events = 8; limits.mailbox_capacity = 4;
    limits.reliable_event_reserve = 1; limits.max_services = 4;
    limits.max_revoked_authorities_per_component = 8;
    limits.event_block_size = 64; limits.event_block_count = 128;
    size = pxa_runtime_workspace_size(&limits); assert(size);
    workspace = malloc(size); assert(workspace);
    assert(pxa_runtime_init(workspace, size, &limits, &runtime) == PXA_STATUS_OK);
    component = create(1); other = create(2);
    assert(dispatch(component, PXA_VT_SERVICE_ID, PXA_VT_HELLO, NULL, 0) == PXA_STATUS_UNSUPPORTED);
    config.struct_size = sizeof(config);
    config.app_identity = (pxa_bytes_t){(const uint8_t *)"voice.test", 10};
    config.declarations = declarations; config.declaration_count = 5;
    config.max_authorities = 5;
    config.store = (pxa_permission_store_t){sizeof(config.store), NULL, load, save};
    size = pxa_permission_service_workspace_size(&config); assert(size);
    permission_workspace = malloc(size); assert(permission_workspace);
    assert(pxa_permission_service_init(permission_workspace, size, runtime, &config, &permissions) == PXA_STATUS_OK);
    assert(pxa_permission_policy_load(permissions) == PXA_STATUS_OK);
    assert(pxa_permission_service_register(permissions) == PXA_STATUS_OK);
    /* Xiaozhi already holds all four ordinary audio/network permissions.
     * Its optional wake permission must coexist with them as the fifth. */
    assert(acquire_named(component, "audio.capture", "microphone") >> 32);
    assert(acquire_named(component, "audio.playback", "media") >> 32);
    assert(acquire_named(component, "net.client", "web") >> 32);
    assert(acquire_named(component, "net.socket", "outbound") >> 32);
    size = pxa_vt_service_workspace_size(); service_workspace = malloc(size); assert(service_workspace);
    assert(pxa_vt_service_init(service_workspace, size, runtime, permissions, &ops, &service) == PXA_STATUS_OK);
    assert(pxa_vt_service_register(service) == PXA_STATUS_OK);
    memcpy(hello, identity, 16);
    pxa_write_u16(hello + 16, 0); pxa_write_u16(hello + 18, 1);
    pxa_write_u16(hello + 20, 0); pxa_write_u16(hello + 22, 1);
    assert(call(component, PXA_VT_SERVICE_ID, PXA_VT_HELLO, hello, 24) == PXA_STATUS_OK);
    assert(reply.payload.size == 24 && !memcmp(reply.payload.data + 4, identity, 16));
    hello[0] ^= 1;
    assert(call(component, PXA_VT_SERVICE_ID, PXA_VT_HELLO, hello, 24) == PXA_STATUS_UNSUPPORTED);
    backend.profile.revision = 7;
    assert(call(component, PXA_VT_SERVICE_ID, PXA_VT_CAPABILITIES, NULL, 0) == PXA_STATUS_OK);
    assert(pxa_read_u32(reply.payload.data + 8) == 0);
    permission = acquire(component); assert(permission >> 32);
    assert(subscribe(component, permission, 7, NULL) == PXA_STATUS_DENIED);
    pxa_vt_service_set_foreground(service, component);
    assert(subscribe(component, permission, 7, NULL) == PXA_STATUS_UNAVAILABLE);
    backend.profile.ready = 1;
    backend.profile.keyword_count = 1;
    backend.profile.keywords[0].id = 42;
    strcpy(backend.profile.keywords[0].label, "你好小智");
    assert(subscribe(component, permission, 6, NULL) == PXA_STATUS_BAD_STATE);
    assert(call(component, PXA_VT_SERVICE_ID, PXA_VT_LIST_PROFILES, NULL, 0) == PXA_STATUS_OK);
    pxa_write_u32(payload, 1); pxa_write_u32(payload + 4, 7);
    assert(call(component, PXA_VT_SERVICE_ID, PXA_VT_LIST_KEYWORDS, payload, 8) == PXA_STATUS_OK);
    assert(pxa_read_u32(reply.payload.data + 12) == 42);
    assert(subscribe(component, permission, 7, &handle) == PXA_STATUS_OK);
    assert(subscribe(component, permission, 7, NULL) == PXA_STATUS_BUSY);
    assert(backend.starts == 1 && pxa_vt_service_active(service));
    // Metadata queries cannot overwrite the active subscription's snapshot.
    backend.profile.ready = 0; backend.profile.keyword_count = 0;
    assert(call(component, PXA_VT_SERVICE_ID, PXA_VT_CAPABILITIES, NULL, 0) == PXA_STATUS_OK);
    backend.profile.ready = 1; backend.profile.keyword_count = 1;
    push(1, 1000, 42);
    assert(pxa_vt_service_poll(service, 1100) == PXA_STATUS_OK);
    pop(component);
    assert(reply.opcode == PXA_VT_EVENT && !reply.request_token && reply.payload.size == 40);
    assert(pxa_read_u64(reply.payload.data) == handle && pxa_read_u64(reply.payload.data + 8) == 1);
    push(1, 1000, 42);
    assert(pxa_vt_service_poll(service, 1200) == PXA_STATUS_OK); empty();
    // A full mailbox must retain the next event and retry without re-reading it.
    for (unsigned i = 0; i < 4; ++i)
        assert(pxa_event_post_message(runtime, component, 17, 0x8006, 0,
            (pxa_bytes_t){NULL, 0}, 1, 0) == PXA_STATUS_OK);
    push(2, 2000, 42);
    assert(pxa_vt_service_poll(service, 2100) != PXA_STATUS_OK);
    for (unsigned i = 0; i < 4; ++i) pop(component);
    assert(pxa_vt_service_poll(service, 2200) == PXA_STATUS_OK);
    pop(component); assert(pxa_read_u64(reply.payload.data + 8) == 2); empty();
    previous = handle;
    pxa_vt_service_set_foreground(service, other);
    assert(!backend.enabled && backend.stops == 1 && !pxa_vt_service_active(service));
    assert(pxa_handle_get64(runtime, component, previous, PXA_VT_RESOURCE_TYPE, &resource) == PXA_STATUS_NOT_FOUND);
    assert(subscribe(other, permission, 7, NULL) == PXA_STATUS_NOT_FOUND);
    pxa_vt_service_set_foreground(service, component);
    assert(subscribe(component, permission, 7, &handle) == PXA_STATUS_OK && handle != previous);
    // A stale match produces a failure, never a delayed wake action.
    push(3, 3000, 42);
    assert(pxa_vt_service_poll(service, 3000001) == PXA_STATUS_OK);
    pop(component);
    assert(pxa_read_u32(reply.payload.data + 32) == PXA_VT_FAILED);
    assert((int32_t)pxa_read_u32(reply.payload.data + 36) == PXA_STATUS_TIMED_OUT);
    assert(!backend.enabled);
    assert(pxa_handle_close64(runtime, component, handle) == PXA_STATUS_OK);
    assert(subscribe(component, permission, 7, &handle) == PXA_STATUS_OK);
    backend.error = PXA_STATUS_UNAVAILABLE;
    assert(pxa_vt_service_poll(service, 4000000) == PXA_STATUS_OK);
    pop(component); assert(pxa_read_u32(reply.payload.data + 32) == PXA_VT_FAILED);
    backend.error = PXA_STATUS_OK;
    assert(pxa_handle_close64(runtime, component, handle) == PXA_STATUS_OK);
    assert(subscribe(component, permission, 7, &handle) == PXA_STATUS_OK);
    // Closing the authority must stop detection even without an App callback.
    assert(pxa_handle_close64(runtime, component, permission) == PXA_STATUS_OK);
    assert(!pxa_vt_service_active(service) && !backend.enabled);
    assert(pxa_vt_service_poll(service, 5000000) == PXA_STATUS_OK); empty();
    permission = acquire(component);
    assert(subscribe(component, permission, 7, &handle) == PXA_STATUS_OK);
    pxa_runtime_deinit(runtime);
    assert(!backend.enabled && !pxa_vt_service_active(service));
    pxa_vt_service_deinit(service);
    free(service_workspace); free(permission_workspace); free(workspace);
    assert(pxa_vt_valid_word("你好", strlen("你好")));
    assert(!pxa_vt_valid_word("\xc0\x80", 2));
    assert(!pxa_vt_valid_word("\xed\xa0\x80", 3));
    assert(!pxa_vt_valid_word("\xf4\x90\x80\x80", 4));
    puts("VoiceTrigger Host integration OK");
    return 0;
}
