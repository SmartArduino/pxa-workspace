#pragma once
#include <cstddef>
#include <cstdint>
#include "pxa/status.h"

// Board-owned 16 kHz mono input. Control and PCM reads run on a worker;
// the existing Audio service's OPEN/READ/CLOSE remain nonblocking.
class PxaAudioCaptureInput {
public:
    using Control = bool (*)(void* context, bool enabled);
    // Returns mono sample count, WOULD_BLOCK, or a negative PXA status.
    using Read = int32_t (*)(void* context, int16_t* pcm, size_t samples);
    bool Initialize(Control control, Read read, void* context);
    void StopForShutdown();
private:
    struct State;
    State* state_ = nullptr;
    static pxa_status_t Open(void* context);
    static int32_t ReadPacket(void* context, uint8_t* output, size_t capacity);
    static void Close(void* context);
    static void Task(void* context);
};
