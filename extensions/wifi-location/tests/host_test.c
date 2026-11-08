// SPDX-License-Identifier: Apache-2.0
#include <pxa_ext/wifi_location.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static pxa_runtime_t *runtime;
static pxa_request_token_t token = 1;
static uint8_t response[2048];
static pxa_message_view_t reply;
static pxa_status_t scan_status=PXA_STATUS_OK;
static unsigned reads=0;
static pxa_status_t read_snapshot(void *ctx,pxa_wl_snapshot_t *out) {
    (void)ctx;++reads;out->timestamp_us=1000000;out->count=2;
    memcpy(out->aps[0].bssid,"ABCDEF",6);out->aps[0].rssi=-50;
    strcpy(out->aps[0].ssid,"one");strcpy(out->aps[1].ssid,"two");
    return scan_status;
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

int main(void) {
    pxa_runtime_limits_t limits;pxa_runtime_limits_init(&limits);
    limits.max_components=2;limits.max_services=4;limits.max_requests=4;limits.max_requests_per_component=4;
    limits.max_handles=8;limits.max_events=8;limits.mailbox_capacity=4;limits.reliable_event_reserve=1;
    limits.event_block_size=64;limits.event_block_count=64;limits.max_revoked_authorities_per_component=4;
    size_t size=pxa_runtime_workspace_size(&limits);void *memory=malloc(size);
    assert(pxa_runtime_init(memory,size,&limits,&runtime)==PXA_STATUS_OK);
    pxa_component_t c=create(1),other=create(2);
    const pxa_permission_declaration_t declaration={{(const uint8_t*)PXA_WL_PERMISSION,sizeof(PXA_WL_PERMISSION)-1},{(const uint8_t*)PXA_WL_SCOPE,sizeof(PXA_WL_SCOPE)-1},0};
    pxa_permission_config_t config={0};config.struct_size=sizeof(config);config.app_identity=(pxa_bytes_t){(const uint8_t*)"location.test",13};
    config.declarations=&declaration;config.declaration_count=1;config.max_authorities=2;
    config.store=(pxa_permission_store_t){sizeof(config.store),NULL,load,save};
    size=pxa_permission_service_workspace_size(&config);void *pm=malloc(size);pxa_permission_service_t *permissions;
    assert(pxa_permission_service_init(pm,size,runtime,&config,&permissions)==PXA_STATUS_OK);
    assert(pxa_permission_policy_load(permissions)==PXA_STATUS_OK);assert(pxa_permission_service_register(permissions)==PXA_STATUS_OK);
    size=pxa_wl_workspace_size();void *sm=malloc(size);pxa_wl_service_t *service;
    assert(pxa_wl_init(sm,size,runtime,permissions,read_snapshot,NULL,&service)==PXA_STATUS_OK);assert(pxa_wl_register(service)==PXA_STATUS_OK);
    static const uint8_t id[]=PXA_WL_IDENTITY;uint8_t hello[20];memcpy(hello,id,16);pxa_write_u16(hello+16,0);pxa_write_u16(hello+18,1);
    assert(call(c,PXA_WL_SERVICE_ID,1,hello,20)==PXA_STATUS_OK);assert(reply.payload.size==24);
    hello[0]^=1;assert(call(c,PXA_WL_SERVICE_ID,1,hello,20)==PXA_STATUS_UNSUPPORTED);
    uint8_t payload[8]={0};assert(call(c,PXA_WL_SERVICE_ID,2,payload,8)!=PXA_STATUS_OK);assert(reads==0);
    pxa_handle64_t permission=acquire_named(c,PXA_WL_PERMISSION,PXA_WL_SCOPE);pxa_write_u64(payload,permission);
    assert(call(other,PXA_WL_SERVICE_ID,2,payload,8)!=PXA_STATUS_OK);assert(reads==0);
    assert(call(c,PXA_WL_SERVICE_ID,2,payload,8)==PXA_STATUS_OK);assert(reply.payload.size==36&&pxa_read_u64(reply.payload.data+4)==1000000);
    scan_status=PXA_STATUS_WOULD_BLOCK;assert(call(c,PXA_WL_SERVICE_ID,2,payload,8)==PXA_STATUS_WOULD_BLOCK);
    scan_status=-100;assert(call(c,PXA_WL_SERVICE_ID,2,payload,8)==PXA_STATUS_IO_ERROR);
    assert(pxa_handle_close64(runtime,c,permission)==PXA_STATUS_OK);
    assert(call(c,PXA_WL_SERVICE_ID,2,payload,8)!=PXA_STATUS_OK);
    free(sm);free(pm);free(memory);puts("Wi-Fi location: PASS (identity, permissions, owner, data, pending/error, closed grant)");
}
