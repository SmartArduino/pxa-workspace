// SPDX-License-Identifier: Apache-2.0
#include <pxa_ext/wifi_location.h>
#include <string.h>
struct pxa_wl_service {
    pxa_runtime_t *runtime;
    pxa_permission_service_t *permissions;
    pxa_wl_read_fn read;
    void *context;
};
size_t pxa_wl_workspace_size(void) { return sizeof(pxa_wl_service_t); }
pxa_status_t pxa_wl_init(void *memory, size_t size, pxa_runtime_t *runtime,
    pxa_permission_service_t *permissions, pxa_wl_read_fn read, void *context, pxa_wl_service_t **out) {
    if (!memory || size < sizeof(pxa_wl_service_t) || !runtime || !permissions || !read || !out)
        return PXA_STATUS_INVALID_ARGUMENT;
    pxa_wl_service_t *s = memory;
    *s = (pxa_wl_service_t){runtime, permissions, read, context}; *out = s;
    return PXA_STATUS_OK;
}
static pxa_status_t control(void *ctx, pxa_runtime_t *runtime, pxa_component_t c, const pxa_message_view_t *m) {
    pxa_wl_service_t *s = ctx;
    static const uint8_t identity[] = PXA_WL_IDENTITY;
    static const uint8_t permission[] = PXA_WL_PERMISSION, scope[] = PXA_WL_SCOPE;
    uint8_t out[10 + PXA_WL_MAX_APS * 40]; size_t n = 0;
    pxa_status_t status; uint16_t profile;
    if (!m->request_token || (m->opcode != 1 && m->opcode != 2)) return PXA_STATUS_UNSUPPORTED;
    if (pxa_component_wire_profile(runtime, c, &profile) != PXA_STATUS_OK || profile != 1) return PXA_STATUS_UNSUPPORTED;
    status = pxa_request_begin_reserved(runtime, c, m->request_token, PXA_WL_SERVICE_ID, m->opcode, 0, sizeof(out) + 4);
    if (status != PXA_STATUS_OK) return status;
    status = pxa_request_commit(runtime, c, m->request_token);
    if (status == PXA_STATUS_OK && m->opcode == 1) {
        if (m->payload.size != 20 || memcmp(m->payload.data, identity, 16) ||
            pxa_read_u16(m->payload.data + 16) != 0 || pxa_read_u16(m->payload.data + 18) != 1)
            status = PXA_STATUS_UNSUPPORTED;
        else { memcpy(out, identity, 16); pxa_write_u16(out + 16, 0); pxa_write_u16(out + 18, 1); n = 20; }
    } else if (status == PXA_STATUS_OK) {
        pxa_authority_t authority;
        if (m->payload.size != 8) status = PXA_STATUS_INVALID_ARGUMENT;
        else status = pxa_permission_resolve64(s->permissions, c, pxa_read_u64(m->payload.data),
            (pxa_bytes_t){permission, sizeof(permission)-1}, (pxa_bytes_t){scope, sizeof(scope)-1}, &authority);
        if (status == PXA_STATUS_OK) {
            pxa_wl_snapshot_t sample = {0};
            status = s->read(s->context, &sample);
            if (!pxa_status_is_known(status)) status = PXA_STATUS_IO_ERROR;
            if (status == PXA_STATUS_OK) {
                if (!sample.timestamp_us || sample.count > PXA_WL_MAX_APS) status = PXA_STATUS_PROTOCOL_ERROR;
                else {
                    pxa_write_u64(out, sample.timestamp_us); pxa_write_u16(out + 8, sample.count); n = 10;
                    for (unsigned i = 0; i < sample.count; ++i) {
                        const pxa_wl_ap_t *ap = &sample.aps[i];
                        const char *end = memchr(ap->ssid, 0, sizeof(ap->ssid));
                        if (!end || end == ap->ssid) { status = PXA_STATUS_PROTOCOL_ERROR; break; }
                        const size_t length = (size_t)(end - ap->ssid);
                        memcpy(out + n, ap->bssid, 6); out[n+6] = (uint8_t)ap->rssi; out[n+7] = (uint8_t)length;
                        memcpy(out + n + 8, ap->ssid, length); n += 8 + length;
                    }
                }
            }
        }
    }
    if (pxa_request_complete(runtime, c, m->request_token, status,
        status == PXA_STATUS_OK ? out : NULL, status == PXA_STATUS_OK ? n : 0) != PXA_STATUS_OK)
        (void)pxa_request_cancel(runtime, c, m->request_token);
    return PXA_STATUS_OK;
}
pxa_status_t pxa_wl_register(pxa_wl_service_t *s) {
    if (!s) return PXA_STATUS_INVALID_ARGUMENT;
    pxa_service_ops_t ops = {0}; ops.struct_size = sizeof(ops); ops.service_id = PXA_WL_SERVICE_ID;
    ops.major = 0; ops.minor = 1; ops.context = s; ops.control = control;
    return pxa_service_register(s->runtime, &ops);
}
