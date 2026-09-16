#pragma once

#include <cstddef>
#include <cstdint>

#include <esp_err.h>

namespace pxadb {

enum class TestControlKey : uint8_t {
    kBack,
    kHome,
    kVolumeUp,
    kVolumeDown,
};

struct TestControlCaptureInfo {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride_bytes = 0;
    uint64_t frame_id = 0;
    uint64_t completed_timestamp_us = 0;
    const char* source = "unknown";
};

using TestControlTakeoverCallback = void (*)(void* callback_context);

// Product adapter for logical input injection and final visible-frame capture.
// Configure it before Start()/StartAutostart(); the service copies this table.
struct TestControlAdapter {
    size_t struct_size = sizeof(TestControlAdapter);
    void* context = nullptr;
    uint16_t width = 0;
    uint16_t height = 0;
    bool (*route_pointer_down)(void* context) = nullptr;
    bool (*inject_pointer)(void* context, uint16_t x, uint16_t y,
                           bool pressed) = nullptr;
    bool (*cancel_pointer)(void* context) = nullptr;
    bool (*route_key)(void* context, TestControlKey key) = nullptr;
    bool (*capture_rgb565)(void* context, uint16_t* pixels,
                           size_t pixel_count, bool after_present,
                           TestControlCaptureInfo* info) = nullptr;
    void (*set_input_takeover_callback)(
        void* context, TestControlTakeoverCallback callback,
        void* callback_context) = nullptr;
};

esp_err_t ConfigureTestControl(const TestControlAdapter* adapter);

// Starts the ESP32-S3 USB Serial/JTAG PXADB service. It owns the structured
// control and log stream but never exposes a shell or raw partition access.
esp_err_t Start();

// Starts PXADB from a short-lived retry task without delaying UI startup.
esp_err_t StartAutostart();
void Stop();
bool IsRunning();
bool IsEnabled();
bool SetEnabled(bool enabled);

}  // namespace pxadb
