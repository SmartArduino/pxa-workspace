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
typedef struct { int live, ready; pxa_status_t status; } device_t;
static device_t devices[8];
static unsigned opens, closes, probes, cancels;
static pxa_status_t ports(void *ctx, pxa_peripheral_port_t *out, size_t capacity, size_t *count) {
    (void)ctx; assert(capacity); *out=(pxa_peripheral_port_t){3,2,3,"expansion.i2c"}; *count=1; return PXA_STATUS_OK;
}
static pxa_status_t module(void *ctx, uint16_t id, pxa_peripheral_module_t *out) {
    (void)ctx; (void)id; (void)out; return PXA_STATUS_NOT_FOUND;
}
static pxa_status_t open_device(void *ctx, uint64_t owner, uint16_t id, uint16_t mode,
    uint32_t parameter, uint16_t address, void **output) {
    (void)ctx; assert(owner>>32 && id==3 && mode==3 && parameter==100000 && address==0x20);
    for (unsigned i=0;i<8;++i) if (!devices[i].live) {
        devices[i]=(device_t){1,0,PXA_STATUS_OK}; *output=&devices[i]; ++opens; return PXA_STATUS_OK;
    }
    return PXA_STATUS_RESOURCE_LIMIT;
}
static pxa_status_t ready_device(void *ctx, void *native) {
    (void)ctx; device_t *d=native; assert(d->live); ++probes;
    return d->ready ? d->status : PXA_STATUS_WOULD_BLOCK;
}
static int32_t io(void *ctx, void *native, uint32_t op, uint8_t *data, size_t size) {
    (void)ctx; (void)native; (void)op; (void)data; (void)size; assert(0); return -1;
}
static pxa_status_t start(void *ctx, void *native, uint64_t cookie, const uint8_t *tx, uint16_t n, uint16_t rx) {
    (void)ctx; (void)native; (void)cookie; (void)tx; (void)n; (void)rx; assert(0); return -1;
}
static pxa_status_t poll(void *ctx, void *native, uint64_t cookie, uint8_t *out, size_t *size) {
    (void)ctx; (void)native; (void)cookie; (void)out; (void)size; assert(0); return -1;
}
static void cancel(void *ctx, void *native, uint64_t cookie) { (void)ctx; (void)native; (void)cookie; ++cancels; }
static void close_device(void *ctx, void *native) { (void)ctx; device_t *d=native; assert(d->live); d->live=0; ++closes; }
static void packet(uint8_t out[20], pxa_handle64_t permission) {
    memset(out,0,20); pxa_write_u16(out,3); pxa_write_u16(out+2,3);
    pxa_write_u32(out+4,100000); pxa_write_u16(out+8,0x20); pxa_write_u64(out+12,permission);
}
static void empty(void) { pxa_event_view_t view; assert(pxa_event_peek(runtime,component,&view)==PXA_STATUS_WOULD_BLOCK); }
int main(void) {
    pxa_runtime_limits_t limits; pxa_permission_config_t config={0}; size_t size;
    pxa_peripherals_service_t *service;
    const pxa_permission_declaration_t declaration={
        {(const uint8_t *)PXA_PERIPHERALS_PERMISSION,sizeof(PXA_PERIPHERALS_PERMISSION)-1},
        {(const uint8_t *)"expansion.i2c",13},0};
    const pxa_peripherals_backend_t backend={sizeof(backend),NULL,ports,module,open_device,io,start,poll,cancel,close_device,ready_device};
    pxa_runtime_limits_init(&limits); limits.max_services=3; limits.max_components=2;
    limits.max_requests=8; limits.max_requests_per_component=6; limits.max_handles=8;
    limits.max_events=16; limits.mailbox_capacity=8; limits.event_block_size=64; limits.event_block_count=128;
    size=pxa_runtime_workspace_size(&limits); void *rw=malloc(size); assert(rw);
    assert(pxa_runtime_init(rw,size,&limits,&runtime)==PXA_STATUS_OK);
    component=create(1); other=create(2);
    config.struct_size=sizeof(config); config.app_identity=(pxa_bytes_t){(const uint8_t *)"async.test",10};
    config.declarations=&declaration; config.declaration_count=1; config.max_authorities=2;
    config.store=(pxa_permission_store_t){sizeof(config.store),NULL,load,save};
    size=pxa_permission_service_workspace_size(&config); void *pw=malloc(size); assert(pw);
    assert(pxa_permission_service_init(pw,size,runtime,&config,&permissions)==PXA_STATUS_OK);
    assert(pxa_permission_policy_load(permissions)==PXA_STATUS_OK && pxa_permission_service_register(permissions)==PXA_STATUS_OK);
    size=pxa_peripherals_service_workspace_size(); void *sw=malloc(size); assert(sw);
    assert(pxa_peripherals_service_init(sw,size,runtime,permissions,&backend,&service)==PXA_STATUS_OK);
    assert(pxa_peripherals_service_register(service)==PXA_STATUS_OK);
    // Old native backend structures are copied by their known prefix only.
    void *legacy=malloc(PXA_PERIPHERALS_BACKEND_BASE_SIZE), *lw=malloc(size); assert(legacy && lw);
    memcpy(legacy,&backend,PXA_PERIPHERALS_BACKEND_BASE_SIZE);
    ((pxa_peripherals_backend_t *)legacy)->struct_size=PXA_PERIPHERALS_BACKEND_BASE_SIZE;
    pxa_peripherals_service_t *legacy_service;
    assert(pxa_peripherals_service_init(lw,size,runtime,permissions,legacy,&legacy_service)==PXA_STATUS_OK);
    free(lw); free(legacy);
    uint8_t hello[24]={0}, request[20]; const uint8_t identity[]=PXA_PERIPHERALS_IDENTITY;
    memcpy(hello,identity,16); pxa_write_u16(hello+18,1); pxa_write_u16(hello+22,1);
    assert(call(component,PXA_PERIPHERALS_SERVICE_ID,1,hello,24)==PXA_STATUS_OK);
    pxa_peripherals_service_set_foreground(service,component);
    pxa_handle64_t permission=acquire_named(component,PXA_PERIPHERALS_PERMISSION,"expansion.i2c");
    packet(request,permission);
    assert(dispatch(component,PXA_PERIPHERALS_SERVICE_ID,4,request,20)==PXA_STATUS_OK);
    const pxa_request_token_t first=token-1; empty();
    for(unsigned i=0;i<5;++i) { pxa_peripherals_service_poll(service); empty(); }
    assert(opens==1 && !closes && pxa_peripherals_service_pending(service));
    // Exhaust Core handles while one initialization is pending. Native setup
    // must never be queued when the Host cannot bind its eventual handle.
    pxa_resource_t dummy={0}; pxa_handle64_t held[8]; unsigned count=0;
    while(count<8 && pxa_handle_open64(runtime,component,PXA_RESOURCE_STREAM,0,&dummy,&held[count])==PXA_STATUS_OK) ++count;
    assert(count>0);
    assert(call(component,PXA_PERIPHERALS_SERVICE_ID,4,request,20)==PXA_STATUS_RESOURCE_LIMIT && opens==1);
    for(unsigned i=0;i<count;++i) assert(pxa_handle_close64(runtime,component,held[i])==PXA_STATUS_OK);
    devices[0].ready=1; pxa_peripherals_service_poll(service); pop(component);
    assert(reply.request_token==first && reply.payload.size==12 && pxa_read_u32(reply.payload.data)==0);
    pxa_handle64_t handle=pxa_read_u64(reply.payload.data+4);
    assert(pxa_handle_close64(runtime,component,handle)==PXA_STATUS_OK && closes==1);
    assert(dispatch(component,PXA_PERIPHERALS_SERVICE_ID,4,request,20)==PXA_STATUS_OK); empty();
    devices[0].ready=1; devices[0].status=PXA_STATUS_IO_ERROR;
    pxa_peripherals_service_poll(service); pop(component);
    assert(reply.payload.size==4 && pxa_read_u32(reply.payload.data)==(uint32_t)PXA_STATUS_IO_ERROR && closes==2);
    for(unsigned i=0;i<4;++i) assert(dispatch(component,PXA_PERIPHERALS_SERVICE_ID,4,request,20)==PXA_STATUS_OK);
    const unsigned before_limit=opens;
    assert(call(component,PXA_PERIPHERALS_SERVICE_ID,4,request,20)==PXA_STATUS_RESOURCE_LIMIT && opens==before_limit);
    pxa_peripherals_service_set_foreground(service,other);
    assert(!pxa_peripherals_service_pending(service) && closes==6 && !cancels);
    for(unsigned i=0;i<4;++i) { pop(component); assert(reply.payload.size==4 && pxa_read_u32(reply.payload.data)==(uint32_t)PXA_STATUS_CANCELLED); }
    const unsigned before_late=probes; pxa_peripherals_service_poll(service); assert(probes==before_late); empty();
    pxa_peripherals_service_set_foreground(service,component);
    assert(dispatch(component,PXA_PERIPHERALS_SERVICE_ID,4,request,20)==PXA_STATUS_OK);
    assert(pxa_handle_close64(runtime,component,permission)==PXA_STATUS_OK);
    pop(component); assert(pxa_read_u32(reply.payload.data)==(uint32_t)PXA_STATUS_CANCELLED && closes==7 && !cancels);
    permission=acquire_named(component,PXA_PERIPHERALS_PERMISSION,"expansion.i2c"); packet(request,permission);
    assert(dispatch(component,PXA_PERIPHERALS_SERVICE_ID,4,request,20)==PXA_STATUS_OK);
    assert(pxa_component_request_stop(runtime,component,PXA_STOP_NORMAL)==PXA_STATUS_OK);
    assert(pxa_component_begin_stop(runtime,component)==PXA_STATUS_OK);
    assert(pxa_component_finish_stop(runtime,component)==PXA_STATUS_OK);
    assert(!pxa_peripherals_service_pending(service) && closes==8 && !cancels);
    pxa_runtime_deinit(runtime); free(sw); free(pw); free(rw);
    puts("Async OPEN: deferred readiness, legacy backend, Core reservation, error cleanup, queue limits and revocation passed");
}
