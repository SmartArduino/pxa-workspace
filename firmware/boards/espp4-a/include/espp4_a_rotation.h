// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <driver/ppa.h>

namespace espp4_a {
// The display task is the sole caller. A blocking transaction finishes before
// the native buffer can be handed to DSI; input/output never alias.
class RotationEngine {
public:
    struct Stats { uint32_t hardware = 0, software = 0; };
    RotationEngine() = default;
    ~RotationEngine();
    RotationEngine(const RotationEngine&) = delete;
    RotationEngine& operator=(const RotationEngine&) = delete;
    bool Rotate(const uint16_t* source, uint16_t* output,
                uint32_t native_width, uint32_t native_height, uint8_t rotation);
    void Disable() { disabled_ = true; }
    Stats TakeStats() { const auto result = stats_; stats_ = {}; return result; }
private:
    ppa_client_handle_t client_ = nullptr;
    bool disabled_ = false;
    Stats stats_;
};
} // namespace espp4_a
