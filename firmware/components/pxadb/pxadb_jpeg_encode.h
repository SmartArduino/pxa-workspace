#pragma once
#include "pxadb/pxadb_service.h"

namespace pxadb {
struct JpegEncodeStats {
    size_t capacity = 0;
    uint32_t attempts = 0;
};
// Caller owns the returned heap_caps allocation. Capture pixels stay immutable.
bool EncodeCaptureJpeg(const uint16_t* pixels, const TestControlCaptureInfo& info,
                       uint8_t** output, size_t* output_size,
                       JpegEncodeStats* stats = nullptr);
// Explicit diagnostic: encodes a noisy 256x256 image with the production capacity.
bool MakeJpegSelfTest(uint8_t** output, size_t* output_size, JpegEncodeStats* stats);
}
