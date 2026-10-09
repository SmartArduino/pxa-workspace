#pragma once
#include <cstddef>
#include <cstdint>

using ppa_client_handle_t = void*;
enum { PPA_OPERATION_SRM, PPA_OPERATION_FILL, PPA_DATA_BURST_LENGTH_128,
       PPA_SRM_COLOR_MODE_RGB565, PPA_FILL_COLOR_MODE_RGB565,
       PPA_SRM_ROTATION_ANGLE_0, PPA_TRANS_MODE_BLOCKING };
struct ppa_client_config_t { int oper_type; uint32_t max_pending_trans_num; int data_burst_length; };
struct color_pixel_argb8888_data_t { uint8_t a, r, g, b; };
struct ppa_in_pic_blk_config_t {
    void* buffer;
    uint32_t pic_w, pic_h, block_w, block_h;
    int srm_cm;
};
struct ppa_out_pic_blk_config_t {
    void* buffer;
    size_t buffer_size;
    uint32_t pic_w, pic_h, block_offset_x, block_offset_y;
    union { int srm_cm; int fill_cm; };
};
struct ppa_srm_oper_config_t {
    ppa_in_pic_blk_config_t in;
    ppa_out_pic_blk_config_t out;
    int rotation_angle;
    float scale_x, scale_y;
    bool byte_swap;
    int mode;
};
struct ppa_fill_oper_config_t {
    ppa_out_pic_blk_config_t out;
    uint32_t fill_block_w, fill_block_h;
    color_pixel_argb8888_data_t fill_argb_color;
    int mode;
};
int ppa_register_client(const ppa_client_config_t*, ppa_client_handle_t*);
int ppa_do_scale_rotate_mirror(ppa_client_handle_t, const ppa_srm_oper_config_t*);
int ppa_do_fill(ppa_client_handle_t, const ppa_fill_oper_config_t*);
