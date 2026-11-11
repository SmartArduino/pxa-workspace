// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>

namespace espp4_a {
class UsbDownloadTrigger {
public:
    void SetBitRate(uint32_t rate) { rate_ = rate; }
    bool SetDtr(bool dtr) {
        const bool download = rate_ == 1200 && dtr_ && !dtr;
        dtr_ = dtr;
        return download;
    }
private:
    uint32_t rate_ = 115200;
    bool dtr_ = false;
};
}
