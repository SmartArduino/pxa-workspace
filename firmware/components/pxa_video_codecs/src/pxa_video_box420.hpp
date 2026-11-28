// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>

namespace pxa_video {
// Pack 2x2 box-filtered planar YUV into the PPA's alternating UYY/VYY rows.
// Source dimensions must be multiples of four. Pitch is in output luma pixels.
void pack_box420(const uint8_t *const planes[3], const int32_t strides[3],
                 unsigned width, unsigned height, uint8_t *output, unsigned pitch);
}
