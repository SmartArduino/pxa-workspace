#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "pxa_host_main_loop.h"

static int policy(pxa_esp_host_command_t *cmd, uint32_t prompt, int ime,
                   pxa_stop_reason_t *reason) {
    return pxa_host_main_loop_command(cmd, 20, 30, "publisher:game", prompt, ime, reason);
}

int main(void) {
    pxa_esp_host_command_t cmd = {0};
    pxa_stop_reason_t reason;
    const uint8_t native[] = {PXA_ESP_HOST_CMD_POINTER, PXA_ESP_HOST_CMD_UI_EVENT,
        PXA_ESP_HOST_CMD_CONTROLLER, PXA_ESP_HOST_CMD_TICK, PXA_ESP_HOST_CMD_DRAIN,
        PXA_ESP_HOST_CMD_LEASE_MAINTENANCE, PXA_ESP_HOST_CMD_SENSOR_MAINTENANCE,
        PXA_ESP_HOST_CMD_NET_MAINTENANCE, PXA_ESP_HOST_CMD_WINDOW_MAINTENANCE,
        PXA_ESP_HOST_CMD_SYSTEM_COMPLETE, PXA_ESP_HOST_CMD_SYSTEM_EVENT,
        PXA_ESP_HOST_CMD_MOVE_DATA};
    for (unsigned i = 0; i < sizeof(native); ++i) {
        cmd.type = native[i];
        assert(policy(&cmd, 0, 0, &reason) == PXA_HOST_LOOP_PROCESS);
    }
    cmd.type = PXA_ESP_HOST_CMD_STOP;
    cmd.payload.stop.host_instance_id = 19;
    assert(policy(&cmd, 0, 0, &reason) == PXA_HOST_LOOP_SKIP);
    cmd.payload.stop.host_instance_id = 20; cmd.payload.stop.reason = PXA_STOP_RESOURCE_PRESSURE;
    assert(policy(&cmd, 0, 0, &reason) == PXA_HOST_LOOP_EXIT && reason == PXA_STOP_RESOURCE_PRESSURE);
    cmd.type = PXA_ESP_HOST_CMD_BACK; cmd.payload.instance.instance_id = 29;
    assert(policy(&cmd, 0, 0, &reason) == PXA_HOST_LOOP_SKIP);
    cmd.payload.instance.instance_id = 30;
    assert(policy(&cmd, 0, 0, &reason) == PXA_HOST_LOOP_EXIT);
    cmd.type = PXA_ESP_HOST_CMD_START;
    assert(policy(&cmd, 0, 0, &reason) == PXA_HOST_LOOP_EXIT && reason == PXA_STOP_REPLACED);
    cmd.type = PXA_ESP_HOST_CMD_SHUTDOWN;
    assert(policy(&cmd, 0, 0, &reason) == PXA_HOST_LOOP_EXIT && reason == PXA_STOP_SHUTDOWN);
    cmd.type = PXA_ESP_HOST_CMD_PERMISSION_DECISION; cmd.payload.permission_decision.prompt_id = 5;
    assert(policy(&cmd, 5, 0, &reason) == PXA_HOST_LOOP_PROCESS);
    assert(policy(&cmd, 0, 0, &reason) == PXA_HOST_LOOP_EXIT);
    assert(policy(&cmd, 6, 0, &reason) == PXA_HOST_LOOP_EXIT);
    cmd.payload.permission_decision.prompt_id = 0;
    assert(policy(&cmd, 0, 0, &reason) == PXA_HOST_LOOP_EXIT);
    cmd.type = PXA_ESP_HOST_CMD_PERMISSION_SET;
    strcpy(cmd.payload.permission_set.identity, "publisher:other");
    assert(policy(&cmd, 0, 0, &reason) == PXA_HOST_LOOP_SKIP);
    strcpy(cmd.payload.permission_set.identity, "publisher:game");
    assert(policy(&cmd, 0, 0, &reason) == PXA_HOST_LOOP_PROCESS);
    cmd.type = PXA_ESP_HOST_CMD_UNRESPONSIVE_STOP;
    assert(policy(&cmd, 0, 0, &reason) == PXA_HOST_LOOP_EXIT);
    strcpy(cmd.payload.identity.identity, "publisher:old-game");
    assert(policy(&cmd, 0, 0, &reason) == PXA_HOST_LOOP_SKIP);
    cmd.type = PXA_ESP_HOST_CMD_IME_INPUT;
    assert(policy(&cmd, 0, 0, &reason) == PXA_HOST_LOOP_SKIP);
    assert(policy(&cmd, 0, 1, &reason) == PXA_HOST_LOOP_EXIT);
    cmd.type = PXA_ESP_HOST_CMD_SCHEDULER_MAINTENANCE;
    assert(policy(&cmd, 0, 0, &reason) == PXA_HOST_LOOP_SKIP);
    cmd.type = PXA_ESP_HOST_CMD_STORE_INSTALL;
    assert(policy(&cmd, 0, 0, &reason) == PXA_HOST_LOOP_EXIT);
    cmd.type = 255;
    assert(policy(&cmd, 0, 0, &reason) == PXA_HOST_LOOP_EXIT && reason == PXA_STOP_POLICY);
    puts("PASS: native maintenance allowed, old-instance commands ignored, lifecycle mutations retained for owner teardown");
    return 0;
}
