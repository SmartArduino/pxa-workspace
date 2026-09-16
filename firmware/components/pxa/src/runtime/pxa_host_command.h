#ifndef PXA_HOST_COMMAND_H
#define PXA_HOST_COMMAND_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 64 publisher-root hex bytes, ':', 64-byte App ID, NUL. */
#define PXA_HOST_COMMAND_MAX_IDENTITY_BYTES 130u

typedef enum {
    PXA_ESP_HOST_CMD_START = 1,
    PXA_ESP_HOST_CMD_BACK,
    PXA_ESP_HOST_CMD_POINTER,
    PXA_ESP_HOST_CMD_UI_EVENT,
    PXA_ESP_HOST_CMD_TICK,
    PXA_ESP_HOST_CMD_DRAIN,
    PXA_ESP_HOST_CMD_LEASE_MAINTENANCE,
    PXA_ESP_HOST_CMD_SENSOR_MAINTENANCE,
    PXA_ESP_HOST_CMD_NET_MAINTENANCE,
    PXA_ESP_HOST_CMD_SCHEDULER_MAINTENANCE,
    PXA_ESP_HOST_CMD_WINDOW_MAINTENANCE,
    PXA_ESP_HOST_CMD_PERMISSION_DECISION,
    PXA_ESP_HOST_CMD_PERMISSION_SET,
    PXA_ESP_HOST_CMD_UNRESPONSIVE_STOP,
    PXA_ESP_HOST_CMD_CONTROLLER,
    PXA_ESP_HOST_CMD_STOP,
    PXA_ESP_HOST_CMD_SYSTEM_COMPLETE,
    PXA_ESP_HOST_CMD_SYSTEM_EVENT,
} pxa_esp_host_command_type_t;

typedef struct {
    uint64_t instance_id;
    uint64_t timestamp_us;
    uint32_t surface;
    uint32_t node;
    int32_t x;
    int32_t y;
    uint8_t id;
    uint8_t phase;
} pxa_host_pointer_event_t;

typedef struct {
    uint64_t instance_id;
    uint64_t timestamp_us;
    uint32_t surface;
    uint32_t node;
    int32_t value;
    uint16_t kind;
    uint16_t flags;
} pxa_host_ui_event_t;

typedef struct {
    uint64_t instance_id;
    uint64_t timestamp_us;
    uint32_t buttons;
    uint8_t controller;
    uint8_t connected;
} pxa_host_controller_event_t;

typedef struct {
    char identity[PXA_HOST_COMMAND_MAX_IDENTITY_BYTES];
} pxa_host_identity_command_t;

typedef struct {
    void *response;
} pxa_host_system_complete_command_t;

typedef struct {
    void *event;
} pxa_host_system_event_command_t;

typedef struct {
    uint64_t instance_id;
} pxa_host_instance_command_t;

typedef struct {
    uint32_t generation;
    uint8_t slot;
} pxa_host_tick_command_t;

typedef struct {
    uint32_t prompt_id;
    uint8_t granted;
} pxa_host_permission_decision_command_t;

typedef struct {
    char identity[PXA_HOST_COMMAND_MAX_IDENTITY_BYTES];
    uint16_t permission_index;
    uint8_t granted;
} pxa_host_permission_set_command_t;

typedef union {
    pxa_host_identity_command_t identity;
    pxa_host_instance_command_t instance;
    pxa_host_pointer_event_t pointer;
    pxa_host_ui_event_t ui_event;
    pxa_host_controller_event_t controller;
    pxa_host_tick_command_t tick;
    pxa_host_permission_decision_command_t permission_decision;
    pxa_host_permission_set_command_t permission_set;
    pxa_host_system_complete_command_t system_complete;
    pxa_host_system_event_command_t system_event;
} pxa_esp_host_command_payload_t;

typedef struct {
    uint8_t type;
    pxa_esp_host_command_payload_t payload;
} pxa_esp_host_command_t;

typedef char pxa_host_command_size_must_not_exceed_144_bytes[
    sizeof(pxa_esp_host_command_t) <= 144u ? 1 : -1];

#ifdef __cplusplus
}
#endif

#endif
