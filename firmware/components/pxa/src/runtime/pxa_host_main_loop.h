#ifndef PXA_HOST_MAIN_LOOP_H
#define PXA_HOST_MAIN_LOOP_H
#include "pxa_host_command.h"
#include "pxa/runtime.h"
#include <string.h>

enum { PXA_HOST_LOOP_PROCESS, PXA_HOST_LOOP_SKIP, PXA_HOST_LOOP_EXIT };

/* Peek before removing. Lifecycle/package commands remain queued until the
 * native Guest call leaves; stale-instance commands cannot cancel a new game. */
static inline int pxa_host_main_loop_command(const pxa_esp_host_command_t *cmd,
    uint64_t package_instance, uint64_t ui_instance, const char *identity,
    uint32_t runtime_prompt, int ime_active, pxa_stop_reason_t *reason) {
    *reason = PXA_STOP_NORMAL;
    switch (cmd->type) {
    case PXA_ESP_HOST_CMD_POINTER:
    case PXA_ESP_HOST_CMD_UI_EVENT:
    case PXA_ESP_HOST_CMD_CONTROLLER:
    case PXA_ESP_HOST_CMD_TICK:
    case PXA_ESP_HOST_CMD_DRAIN:
    case PXA_ESP_HOST_CMD_LEASE_MAINTENANCE:
    case PXA_ESP_HOST_CMD_SENSOR_MAINTENANCE:
    case PXA_ESP_HOST_CMD_NET_MAINTENANCE:
    case PXA_ESP_HOST_CMD_WINDOW_MAINTENANCE:
    case PXA_ESP_HOST_CMD_SYSTEM_COMPLETE:
    case PXA_ESP_HOST_CMD_SYSTEM_EVENT:
    case PXA_ESP_HOST_CMD_MOVE_DATA:
    case PXA_ESP_HOST_CMD_MOVE_PACKAGE:
    case PXA_ESP_HOST_CMD_MOVE_RESOURCES:
    case PXA_ESP_HOST_CMD_INSTALL_FILE:
        return PXA_HOST_LOOP_PROCESS;
    case PXA_ESP_HOST_CMD_PERMISSION_SET:
        return strcmp(cmd->payload.permission_set.identity, identity) == 0
            ? PXA_HOST_LOOP_PROCESS : PXA_HOST_LOOP_SKIP;
    case PXA_ESP_HOST_CMD_PERMISSION_DECISION:
        return runtime_prompt && cmd->payload.permission_decision.prompt_id == runtime_prompt
            ? PXA_HOST_LOOP_PROCESS : PXA_HOST_LOOP_EXIT;
    case PXA_ESP_HOST_CMD_SCHEDULER_MAINTENANCE:
        return PXA_HOST_LOOP_SKIP;
    case PXA_ESP_HOST_CMD_IME_INPUT:
        return ime_active ? PXA_HOST_LOOP_EXIT : PXA_HOST_LOOP_SKIP;
    case PXA_ESP_HOST_CMD_STOP:
        if (!cmd->payload.stop.host_instance_id || cmd->payload.stop.host_instance_id != package_instance)
            return PXA_HOST_LOOP_SKIP;
        *reason = cmd->payload.stop.reason;
        return PXA_HOST_LOOP_EXIT;
    case PXA_ESP_HOST_CMD_BACK:
        return cmd->payload.instance.instance_id && cmd->payload.instance.instance_id == ui_instance
            ? PXA_HOST_LOOP_EXIT : PXA_HOST_LOOP_SKIP;
    case PXA_ESP_HOST_CMD_UNRESPONSIVE_STOP:
        return strcmp(cmd->payload.identity.identity, identity) == 0
            ? PXA_HOST_LOOP_EXIT : PXA_HOST_LOOP_SKIP;
    case PXA_ESP_HOST_CMD_START:
        *reason = PXA_STOP_REPLACED;
        return PXA_HOST_LOOP_EXIT;
    case PXA_ESP_HOST_CMD_SHUTDOWN:
        *reason = PXA_STOP_SHUTDOWN;
        return PXA_HOST_LOOP_EXIT;
    default:
        *reason = PXA_STOP_POLICY;
        return PXA_HOST_LOOP_EXIT;
    }
}
#endif
