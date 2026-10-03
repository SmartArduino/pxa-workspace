#include "pxa_board_common/display_profile.h"

namespace pxa_board_common {

void ApplyDisplayProfile(const DisplayProfile& profile,
                         pxsys_display_profile_t* output) {
    if (output == nullptr) return;
    pxsys_display_profile_init(output, profile.width, profile.height);
    output->shape = profile.shape;
    output->corner_radii = profile.corner_radii;
    output->safe_insets = profile.safe_insets;
}

}  // namespace pxa_board_common
