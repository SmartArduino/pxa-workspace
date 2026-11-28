// SPDX-License-Identifier: Apache-2.0
#include "espp4_a_rotation.h"
#include <cstring>
#include <limits>
#include <esp_log.h>
#include <pxa/display_rotation.h>
#include <pxa_board_common/ppa_rgb565.h>

namespace espp4_a {
RotationEngine::~RotationEngine() {
    if (client_) (void)ppa_unregister_client(client_);
}

bool RotationEngine::Rotate(const uint16_t* source, uint16_t* output,
    uint32_t width, uint32_t height, uint8_t rotation) {
    if (!source || !output || source == output || !width || !height || rotation > 3 ||
        uint64_t(width) * height > std::numeric_limits<uint32_t>::max() / 2u) return false;
    const uint32_t bytes = width * height * 2u;
    if (!rotation) {
        std::memcpy(output, source, bytes);
        return false;
    }
    if (!client_ && !disabled_) {
        ppa_client_config_t config{};
        config.oper_type = PPA_OPERATION_SRM;
        config.max_pending_trans_num = 1;
        config.data_burst_length = PPA_DATA_BURST_LENGTH_128;
        const auto status = ppa_register_client(&config, &client_);
        if (status != ESP_OK) {
            disabled_ = true;
            ESP_LOGW("espp4_rotation", "PPA unavailable (%s); using software rotation", esp_err_to_name(status));
        }
    }
    if (!disabled_) {
        // PXA uses clockwise quarter turns, PPA uses counter-clockwise angles.
        constexpr ppa_srm_rotation_angle_t angles[] = {
            PPA_SRM_ROTATION_ANGLE_0, PPA_SRM_ROTATION_ANGLE_270,
            PPA_SRM_ROTATION_ANGLE_180, PPA_SRM_ROTATION_ANGLE_90};
        const auto logical_width = pxa_display_logical_width(width, height, rotation);
        const auto status = pxa::board::RotatePpaRgb565(client_, source, logical_width, logical_width,
            pxa_display_logical_height(width, height, rotation), output, angles[rotation], false);
        if (status == ESP_OK) { ++stats_.hardware; return true; }
        disabled_ = true;
        ESP_LOGW("espp4_rotation", "PPA rotation rejected (%s); using software rotation", esp_err_to_name(status));
    }
    // Invalid alignment or limited DMA resources must not make an otherwise
    // supported app lose its orientation or fail to display.
    pxa_display_rgb565_to_native(source, output, width, height, rotation);
    ++stats_.software;
    return false;
}
} // namespace espp4_a
