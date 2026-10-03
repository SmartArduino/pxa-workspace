#include "board_sensecap_watcher.h"
#include "pxa_board.h"

#include <pxa_board_common/display_profile.h>
#include <pxa_board_common/register.h>

#include "pxa_board_api.h"
#include "watcher_hardware.h"

namespace {
SensecapWatcherHardware g_hardware;

bool Initialize(void*) {
    return g_hardware.Initialize();
}

lv_display_t* Display(void*) {
    return g_hardware.display();
}

void DisplayProfile(void*, pxsys_display_profile_t* output) {
    static constexpr pxa_board_common::DisplayProfile kProfile = {
        .width = 412,
        .height = 412,
        .shape = PXSYS_DISPLAY_SHAPE_CIRCLE,
        .corner_radii = {206, 206, 206, 206},
        .safe_insets = {60, 60, 60, 60},
    };
    pxa_board_common::ApplyDisplayProfile(kProfile, output);
}

pxsys_status_t SetNetworkEnabled(void*, pxsys_network_type_t network,
                                 uint8_t enabled) {
    if (network != PXSYS_NETWORK_WIFI) return PXSYS_STATUS_UNSUPPORTED;
    g_hardware.SetWifiEnabled(enabled != 0);
    return PXSYS_STATUS_OK;
}

pxsys_status_t SetLevel(void*, pxsys_level_control_t control, uint8_t percent) {
    if (control == PXSYS_LEVEL_CONTROL_VOLUME) {
        g_hardware.SetVolume(percent);
        return PXSYS_STATUS_OK;
    }
    if (control == PXSYS_LEVEL_CONTROL_BRIGHTNESS) {
        g_hardware.SetBrightness(percent);
        return PXSYS_STATUS_OK;
    }
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

void ShowInitialFrame(void*) {
    g_hardware.ShowInitialFrame();
}

bool ConfigureDiagnostics(void*) {
    return g_hardware.ConfigurePxadbControls();
}

bool CaptureDisplayedRgb565(void*, uint16_t* pixels, size_t pixel_count) {
    return g_hardware.CaptureRgb565(pixels, pixel_count, false, nullptr);
}

void SetIdleDim(void*, bool enabled, uint8_t percent) {
    g_hardware.SetIdleDim(enabled, percent);
}

void IdleScreenOff(void*) {
    g_hardware.AutoScreenOff();
}

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
};
}  // namespace

extern "C" bool board_sensecap_watcher_register(void) {
    return pxa_board_register(&kPort);
}

PXA_BOARD_REGISTER_SELECTED(board_sensecap_watcher_register)
