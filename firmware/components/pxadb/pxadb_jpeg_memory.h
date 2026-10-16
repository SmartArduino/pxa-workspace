#pragma once

namespace pxadb {
// Capture-only policy; image decoders on other tasks retain vendor defaults.
class JpegMemoryScope {
public:
    JpegMemoryScope();
    ~JpegMemoryScope();
    JpegMemoryScope(const JpegMemoryScope&) = delete;
    JpegMemoryScope& operator=(const JpegMemoryScope&) = delete;
private:
    bool previous_;
};
}
