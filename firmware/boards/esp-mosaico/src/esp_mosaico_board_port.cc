#include "pxa_board.h"

#include "esp_mosaico_hardware.h"
#include "pxa_board_api.h"
#include <esp_lv_adapter.h>

namespace {

EspMosaicoHardware g_hardware;

bool Initialize(void*) { return g_hardware.Initialize(); }
lv_display_t* Display(void*) { return g_hardware.display(); }
void DisplayProfile(void*, pxsys_display_profile_t* output) {
    if (output == nullptr) return;
    pxsys_display_profile_init(output, mosaico_board::kWidth,
                               mosaico_board::kHeight);
    // 480 pixels across approximately 40 mm: 480 * 25.4 / 40 = 305 DPI.
    output->density_dpi = 305;
    output->safe_insets = {12, 12, 12, 12};
    output->shape = PXSYS_DISPLAY_SHAPE_ROUNDED_RECTANGLE;
    output->corner_radii = {58, 58, 58, 58};
}
pxsys_status_t SetNetworkEnabled(void*, pxsys_network_type_t network,
                                 uint8_t enabled) {
    if (network != PXSYS_NETWORK_WIFI) return PXSYS_STATUS_UNSUPPORTED;
    return g_hardware.SetWifiEnabled(enabled != 0)
        ? PXSYS_STATUS_OK : PXSYS_STATUS_UNSUPPORTED;
}
pxsys_status_t SetLevel(void*, pxsys_level_control_t control, uint8_t percent) {
    if (control == PXSYS_LEVEL_CONTROL_VOLUME)
        return g_hardware.SetVolume(percent)
            ? PXSYS_STATUS_OK : PXSYS_STATUS_UNSUPPORTED;
    if (control == PXSYS_LEVEL_CONTROL_BRIGHTNESS)
        return g_hardware.SetBrightness(percent)
            ? PXSYS_STATUS_OK : PXSYS_STATUS_UNSUPPORTED;
    return PXSYS_STATUS_UNSUPPORTED;
}
bool PerformanceGet(void*, pxsys_reference_performance_option_t option) {
    return pxa_board_performance_get(option);
}
bool PerformanceSet(void*, pxsys_reference_performance_option_t option,
                    bool enabled) {
    return pxa_board_performance_set(option, enabled);
}
void SystemReady(void*, pxsys_standard_system_t* system,
                 pxsys_reference_lvgl_t* reference_ui) {
    g_hardware.AttachSystem(system, reference_ui);
}
void ShowInitialFrame(void*) { g_hardware.ShowInitialFrame(); }
bool CaptureDisplayedRgb565(void*, uint16_t* pixels, size_t pixel_count) {
    return g_hardware.CaptureDisplayedRgb565(pixels, pixel_count);
}
void SetIdleDim(void*, bool enabled, uint8_t percent) {
    g_hardware.SetIdleDim(enabled, percent);
}
void IdleScreenOff(void*) { g_hardware.IdleScreenOff(); }
bool LockDisplay(void*, uint32_t timeout_ms) {
    return esp_lv_adapter_lock(static_cast<int32_t>(timeout_ms)) == ESP_OK;
}
void UnlockDisplay(void*) { esp_lv_adapter_unlock(); }
bool ConfigureDiagnostics(void*) { return g_hardware.ConfigurePxadbControls(); }

const pxa_board_port_t kPort = {
    .struct_size = sizeof(pxa_board_port_t),
    .context = nullptr,
    .initialize = Initialize,
    .display = Display,
    .display_profile = DisplayProfile,
    .set_network_enabled = SetNetworkEnabled,
    .set_level = SetLevel,
    .performance_get = PerformanceGet,
    .performance_set = PerformanceSet,
    .system_ready = SystemReady,
    .show_initial_frame = ShowInitialFrame,
    .configure_diagnostics = ConfigureDiagnostics,
    .capture_displayed_rgb565 = CaptureDisplayedRgb565,
    .set_idle_dim = SetIdleDim,
    .idle_screen_off = IdleScreenOff,
    .lock_display = LockDisplay,
    .unlock_display = UnlockDisplay,
    .initialize_before_storage = true,
};

}

extern "C" bool pxa_board_register_selected(void) {
    return pxa_board_register(&kPort);
}
