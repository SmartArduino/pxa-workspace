// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <pxa/pxa_peripherals.h>
#include <pxa_ext/vision_frame.h>
// A board-owned camera, accessed only from the worker after exact-scope
// foreground permission. prepare/read/finish may block for a bounded driver
// deadline; Host callbacks never perform driver I/O. finish must release all
// session state even after a failed prepare/read. One physical session only.
struct PxaBoardVisionDriver {
    void* context;
    bool jpeg, inference;
    pxa_status_t (*prepare)(void*, uint16_t format);
    pxa_status_t (*read)(void*, uint8_t*, size_t capacity, size_t* size);
    void (*finish)(void*);
};
bool pxa_board_vision_port_initialize(const PxaBoardVisionDriver*);
