// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <pxa/status.h>
#include <pxa_ext/vision_frame.h>
pxa_status_t WatcherDecodeVisionReply(char* line, size_t length, uint16_t format,
    uint8_t* output, size_t capacity, size_t* size);
