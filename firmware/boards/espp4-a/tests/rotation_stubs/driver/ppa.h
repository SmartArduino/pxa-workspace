// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_NO_MEM = 257;
inline const char* esp_err_to_name(int) { return "test error"; }
using ppa_client_handle_t = struct ppa_client_t*;
enum ppa_operation_t { PPA_OPERATION_SRM };
enum ppa_data_burst_length_t { PPA_DATA_BURST_LENGTH_128 };
enum ppa_srm_rotation_angle_t {
    PPA_SRM_ROTATION_ANGLE_0, PPA_SRM_ROTATION_ANGLE_90,
    PPA_SRM_ROTATION_ANGLE_180, PPA_SRM_ROTATION_ANGLE_270 };
enum ppa_srm_color_mode_t { PPA_SRM_COLOR_MODE_RGB565 };
enum ppa_alpha_update_mode_t { PPA_ALPHA_NO_CHANGE };
enum ppa_trans_mode_t { PPA_TRANS_MODE_BLOCKING };
struct ppa_client_config_t {
    ppa_operation_t oper_type;
    uint32_t max_pending_trans_num;
    ppa_data_burst_length_t data_burst_length;
};
struct ppa_srm_oper_config_t {
    struct { const void* buffer; uint32_t pic_w, pic_h, block_w, block_h; ppa_srm_color_mode_t srm_cm; } in;
    struct { void* buffer; uint32_t buffer_size, pic_w, pic_h; ppa_srm_color_mode_t srm_cm; } out;
    ppa_srm_rotation_angle_t rotation_angle;
    float scale_x, scale_y;
    ppa_alpha_update_mode_t alpha_update_mode;
    ppa_trans_mode_t mode;
};
extern "C" {
esp_err_t ppa_register_client(const ppa_client_config_t*, ppa_client_handle_t*);
esp_err_t ppa_unregister_client(ppa_client_handle_t);
esp_err_t ppa_do_scale_rotate_mirror(ppa_client_handle_t, const ppa_srm_oper_config_t*);
}
