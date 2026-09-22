#include "pxa_board.h"

#include "esp32s31_korvo_1_config.h"
#include "esp32s31_korvo_1_hardware.h"
#include "pxa_board_api.h"

namespace {
Esp32S31Korvo1Hardware g_hardware;

bool Initialize(void*) { return g_hardware.Initialize(); }
lv_display_t* Display(void*) { return g_hardware.display(); }
void DisplayProfile(void*, pxsys_display_profile_t* output) {
    if (output == nullptr) return;
    pxsys_display_profile_init(output, KORVO_UI_WIDTH, KORVO_UI_HEIGHT);
    output->shape = PXSYS_DISPLAY_SHAPE_RECTANGLE;
}
pxsys_status_t SetNetworkEnabled(void*, pxsys_network_type_t network,
                                 uint8_t enabled) {
    if (network != PXSYS_NETWORK_WIFI) return PXSYS_STATUS_UNSUPPORTED;
    g_hardware.SetWifiEnabled(enabled != 0);
    return PXSYS_STATUS_OK;
}
pxsys_status_t SetLevel(void*, pxsys_level_control_t control, uint8_t percent) {
    if (control != PXSYS_LEVEL_CONTROL_VOLUME) return PXSYS_STATUS_UNSUPPORTED;
    g_hardware.SetVolume(percent);
    return PXSYS_STATUS_OK;
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

bool ConfigureDiagnostics(void*) {
    return g_hardware.ConfigurePxadbControls();
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
    .show_initial_frame = nullptr,
    .configure_diagnostics = ConfigureDiagnostics,
};
}

extern "C" bool pxa_board_register_selected(void) {
    return pxa_board_register(&kPort);
}
