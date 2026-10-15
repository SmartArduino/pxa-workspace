#include "esp_mosaico_hardware.h"
#include "esp_mosaico_transfer.h"

#include <algorithm>
#include <cstring>
#include <esp_lv_adapter.h>
#include <esp_timer.h>
#include <pxa/pxa_host.h>

bool EspMosaicoHardware::RouteInjectedPointerDown() {
    if (display_ == nullptr || esp_lv_adapter_lock(1000) != ESP_OK) return false;
    lv_lock();
    const bool consume = !panel_on_.load();
    lv_display_trigger_activity(display_);
    if (consume) {
        wake_pending_ = true;
    }
    lv_unlock();
    esp_lv_adapter_unlock();
    return !consume;
}

bool EspMosaicoHardware::InjectPointer(uint8_t id, uint16_t x, uint16_t y, bool pressed) {
    if (touch_input_ == nullptr || secondary_touch_input_ == nullptr) return false;
    portENTER_CRITICAL(&touch_lock_);
    const bool result = input_.Inject(id, x, y, pressed, esp_timer_get_time());
    portEXIT_CRITICAL(&touch_lock_);
    return result;
}

bool EspMosaicoHardware::CancelInjectedPointer(uint8_t id) {
    portENTER_CRITICAL(&touch_lock_);
    const bool result = input_.Cancel(id, esp_timer_get_time());
    portEXIT_CRITICAL(&touch_lock_);
    return result;
}

bool EspMosaicoHardware::RouteInjectedKey(pxadb::TestControlKey key) {
    if (!RouteInjectedPointerDown()) return true;
    switch (key) {
        case pxadb::TestControlKey::kHome:
            home_pending_.store(true);
            return true;
        case pxadb::TestControlKey::kBack:
            back_pending_.store(true);
            return true;
        case pxadb::TestControlKey::kVolumeUp:
        case pxadb::TestControlKey::kVolumeDown: {
            const bool up = key == pxadb::TestControlKey::kVolumeUp;
            if (pxa_host_captures_volume_keys()) {
                const bool posted = pxa_host_post_key(
                    up ? PXA_HOST_KEY_VOLUME_UP : PXA_HOST_KEY_VOLUME_DOWN);
                const bool released = pxa_host_post_key(up
                    ? PXA_HOST_KEY_VOLUME_UP_RELEASED : PXA_HOST_KEY_VOLUME_DOWN_RELEASED);
                return posted && released;
            }
            const int volume = audio_.volume();
            return SetVolume(static_cast<uint8_t>(std::clamp(volume + (up ? 10 : -10), 0, 100)));
        }
    }
    return false;
}

bool EspMosaicoHardware::CaptureRgb565(
    uint16_t* pixels, size_t pixel_count, bool after_present,
    pxadb::TestControlCaptureInfo* info) {
    const size_t required = mosaico_board::kWidth * mosaico_board::kHeight;
    if (pixels == nullptr || info == nullptr || pixel_count < required ||
        display_ == nullptr || esp_lv_adapter_lock(1000) != ESP_OK) return false;
    lv_lock();
    const uint32_t initial_sequence = completed_frames_;
    if (after_present) {
        lv_obj_invalidate(lv_display_get_screen_active(display_));
        lv_refr_now(display_);
    }
    WaitForFlush();
    xSemaphoreTake(panel_mutex_, portMAX_DELAY);
    const bool valid = displayed_frame_valid_ &&
        (!after_present || completed_frames_ != initial_sequence);
    if (valid) {
        mosaico_board::CaptureScanoutRgb565(scanout_pixels_, pixels);
        info->width = mosaico_board::kWidth;
        info->height = mosaico_board::kHeight;
        info->stride_bytes = mosaico_board::kWidth * sizeof(uint16_t);
        info->frame_id = displayed_frame_id_;
        info->completed_timestamp_us = displayed_timestamp_us_;
        info->source = displayed_source_;
    }
    xSemaphoreGive(panel_mutex_);
    lv_unlock();
    esp_lv_adapter_unlock();
    return valid;
}

bool EspMosaicoHardware::ConfigurePxadbControls() {
    pxadb::PowerControlAdapter power;
    power.context = this;
    power.power_off = [](void* context) {
        return static_cast<EspMosaicoHardware*>(context)->RequestPowerAction(
            PXSYS_REFERENCE_POWER_ACTION_SHUTDOWN);
    };
    const esp_err_t power_result = pxadb::ConfigurePowerControl(&power);
    if (power_result != ESP_OK && power_result != ESP_ERR_NOT_SUPPORTED) return false;
    if (display_ == nullptr || touch_input_ == nullptr || secondary_touch_input_ == nullptr)
        return false;
    pxadb::TestControlAdapter adapter;
    adapter.context = this;
    adapter.width = mosaico_board::kWidth;
    adapter.height = mosaico_board::kHeight;
    adapter.max_pointers = EspMosaicoInput::kPointers;
    adapter.route_pointer_down = [](void* context) {
        return static_cast<EspMosaicoHardware*>(context)->RouteInjectedPointerDown();
    };
    adapter.inject_pointer = [](void* context, uint8_t id, uint16_t x, uint16_t y, bool pressed) {
        return static_cast<EspMosaicoHardware*>(context)->InjectPointer(id, x, y, pressed);
    };
    adapter.cancel_pointer = [](void* context, uint8_t id) {
        return static_cast<EspMosaicoHardware*>(context)->CancelInjectedPointer(id);
    };
    adapter.set_input_takeover_callback = [](void* context,
        pxadb::TestControlTakeoverCallback callback, void* callback_context) {
        auto* self = static_cast<EspMosaicoHardware*>(context);
        portENTER_CRITICAL(&self->touch_lock_);
        self->input_takeover_ = callback;
        self->input_takeover_context_ = callback_context;
        portEXIT_CRITICAL(&self->touch_lock_);
    };
    adapter.route_key = [](void* context, pxadb::TestControlKey key) {
        return static_cast<EspMosaicoHardware*>(context)->RouteInjectedKey(key);
    };
    adapter.capture_rgb565 = [](void* context, uint16_t* pixels, size_t count,
                                bool after_present, pxadb::TestControlCaptureInfo* info) {
        return static_cast<EspMosaicoHardware*>(context)->CaptureRgb565(
            pixels, count, after_present, info);
    };
    const esp_err_t result = pxadb::ConfigureTestControl(&adapter);
    return result == ESP_OK || result == ESP_ERR_NOT_SUPPORTED;
}
