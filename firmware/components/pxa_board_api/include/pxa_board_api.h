#ifndef PXA_BOARD_API_H
#define PXA_BOARD_API_H

#include <stdbool.h>
#include <stdint.h>

#include <lvgl.h>
#include <pxsys/reference_lvgl.h>
#include <pxsys/standard_system.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Board ports describe hardware capabilities only. The callbacks are control
 * plane operations; framebuffer, DMA and audio hot paths remain board-owned.
 */
typedef struct {
    uint32_t struct_size;
    void* context;
    bool (*initialize)(void* context);
    lv_display_t* (*display)(void* context);
    void (*display_profile)(void* context, pxsys_display_profile_t* output);
    pxsys_status_t (*set_network_enabled)(void* context,
                                          pxsys_network_type_t network,
                                          uint8_t enabled);
    pxsys_status_t (*set_level)(void* context, pxsys_level_control_t control,
                                uint8_t percent);
    /* Optional persistent diagnostics settings. The board owns the final
     * framebuffer composition, so the system UI only calls this control API. */
    bool (*performance_get)(void* context,
                            pxsys_reference_performance_option_t option);
    bool (*performance_set)(void* context,
                            pxsys_reference_performance_option_t option,
                            bool enabled);
    void (*system_ready)(void* context, pxsys_standard_system_t* system,
                         pxsys_reference_lvgl_t* reference_ui);
    void (*show_initial_frame)(void* context);
    bool (*configure_diagnostics)(void* context);
} pxa_board_port_t;

bool pxa_board_register(const pxa_board_port_t* port);
const pxa_board_port_t* pxa_board_current(void);

/* Shared NVS-backed diagnostics preference used by every board port. */
bool pxa_board_performance_get(pxsys_reference_performance_option_t option);
bool pxa_board_performance_set(pxsys_reference_performance_option_t option,
                               bool enabled);
void pxa_board_performance_note_frame(void);
void pxa_board_performance_draw_rgb565(uint16_t* pixels, uint16_t viewport_width,
                                       uint16_t viewport_height, uint32_t stride,
                                       int32_t origin_x, int32_t origin_y,
                                       uint16_t width, uint16_t height,
                                       bool byte_swapped);

#ifdef __cplusplus
}
#endif

#endif
