#pragma once

#include <cstddef>
#include <cstdint>

namespace mosaico_board {

// Six DMA blocks may legitimately delay new PCM. Allow substantially longer
// than that before declaring that accepted nonzero PCM never reaches DMA.
// Idle silence and short sound effects must not cause a channel restart.
class AudioDmaProgress {
public:
    bool Stalled(bool source_nonzero, uint32_t consumed_nonzero_blocks) {
        if (!source_nonzero || consumed_nonzero_blocks != observed_) {
            observed_ = consumed_nonzero_blocks;
            pending_ = 0;
            return false;
        }
        if (++pending_ < 22) return false;
        pending_ = 0;
        return true;
    }
private:
    uint32_t observed_ = 0;
    uint32_t pending_ = 0;
};

// A successful driver status alone does not guarantee that all PCM was queued.
// After restarting DMA, its old queued data is discarded: retry the complete
// frame once. A persistent fault is reported to the Host rather than spun on.
template <typename Write, typename Restart>
bool SubmitAudioFrame(const void* pcm, size_t bytes, Write write, Restart restart) {
    size_t written = 0;
    if (write(pcm, bytes, &written) && written == bytes) return true;
    if (!restart()) return false;
    written = 0;
    return write(pcm, bytes, &written) && written == bytes;
}

} // namespace mosaico_board
