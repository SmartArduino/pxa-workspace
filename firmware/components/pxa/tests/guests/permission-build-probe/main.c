#include "pxa.h"
#include "pxa_permission.h"

static int32_t request(uint16_t opcode, uint64_t token) {
    static const char name[] = "test.undeclared";
    static const uint8_t scope[] = "build-option";
    uint8_t packet[128]; uint32_t size = 0;
    if (!pxa_permission_build(packet, sizeof(packet), opcode, token, name,
        sizeof(name) - 1, scope, sizeof(scope) - 1, &size)) return PXA_STATUS_INTERNAL;
    return pxa_control_packet(packet, size);
}

int32_t pxa_app_start(const uint8_t *config, uint32_t length) {
    (void)config; (void)length;
    return request(PXA_PERMISSION_CHECK, 1);
}

int32_t pxa_app_on_event(const uint8_t *data, uint32_t length) {
    pxa_event_t event;
    if (!pxa_parse_event(data, length, &event)) return PXA_STATUS_PROTOCOL_ERROR;
    if (event.service != PXA_PERMISSION_SERVICE) return PXA_EVENT_UNHANDLED;
    if (event.token == 1) {
        pxa_permission_check_result_t result;
        if (!pxa_permission_parse_check(&event, 1, &result) || result.status) return PXA_STATUS_INTERNAL;
        if (!result.decision) {
            (void)pxa_log_write(PXA_LOG_LEVEL_INFO, "PERMISSION PROBE policy=on undeclared=deny");
            return PXA_EVENT_HANDLED;
        }
        return request(PXA_PERMISSION_ACQUIRE, 2) ? PXA_STATUS_INTERNAL : PXA_EVENT_HANDLED;
    }
    if (event.token == 2) {
        pxa_permission_acquire_result_t result;
        if (!pxa_permission_parse_acquire(&event, 2, &result) || result.status ||
            pxa_close_handle(result.handle)) return PXA_STATUS_INTERNAL;
        (void)pxa_log_write(PXA_LOG_LEVEL_INFO,
            "PERMISSION PROBE policy=off undeclared=allow acquire=ok close=ok");
        return PXA_EVENT_HANDLED;
    }
    return PXA_EVENT_UNHANDLED;
}

void pxa_app_stop(uint32_t reason) { (void)reason; }
