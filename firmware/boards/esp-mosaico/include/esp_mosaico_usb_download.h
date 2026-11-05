// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

namespace mosaico_board {

class UsbDownloadTrigger {
public:
    void SetBitRate(uint32_t bit_rate) { bit_rate_ = bit_rate; }

    bool SetDtr(bool dtr) {
        const bool download = bit_rate_ == 1200 && dtr_ && !dtr;
        dtr_ = dtr;
        return download;
    }

private:
    uint32_t bit_rate_ = 115200;
    bool dtr_ = false;
};

}
