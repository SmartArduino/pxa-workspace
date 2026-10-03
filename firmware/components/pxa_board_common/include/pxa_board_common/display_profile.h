#pragma once

#include <cstdint>

#include <pxsys/display.h>

namespace pxa_board_common {

/* Panel geometry a board publishes to the PXA session. Keep it in step with
 * the board's `simulator/profiles/<board>.toml` so a desktop run and the
 * device lay out the same way.
 *
 * The nested types are the pxsys ones, so a board can list radii and insets in
 * the same order it always has: {top_left, top_right, bottom_right,
 * bottom_left} and {top, right, bottom, left}. */
struct DisplayProfile {
    uint32_t width = 0;
    uint32_t height = 0;
    pxsys_display_shape_t shape = PXSYS_DISPLAY_SHAPE_RECTANGLE;
    pxsys_corner_radii_t corner_radii = {};
    pxsys_insets_t safe_insets = {};
};

/* Fills the profile the PXA session reads through pxa_board_port_t. Does
 * nothing when `output` is null, like the board callbacks it replaces. */
void ApplyDisplayProfile(const DisplayProfile& profile,
                         pxsys_display_profile_t* output);

}  // namespace pxa_board_common
