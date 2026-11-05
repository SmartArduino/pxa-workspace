// SPDX-License-Identifier: MIT
#include "esp_mosaico_usb_download.h"
#include <cassert>

int main() {
    mosaico_board::UsbDownloadTrigger trigger;
    // Opening, closing and resetting a normal monitor must not enter download.
    assert(!trigger.SetDtr(false));
    assert(!trigger.SetDtr(true));
    assert(!trigger.SetDtr(false));
    trigger.SetBitRate(115200);
    assert(!trigger.SetDtr(true));
    assert(!trigger.SetDtr(false));

    trigger.SetBitRate(1200);
    assert(!trigger.SetDtr(false));
    assert(!trigger.SetDtr(true));
    assert(trigger.SetDtr(false));
    assert(!trigger.SetDtr(false));

    // Changing back to the monitor rate cancels a pending 1200-baud touch.
    assert(!trigger.SetDtr(true));
    trigger.SetBitRate(115200);
    assert(!trigger.SetDtr(false));
    // Changing rate after opening also supports an existing CDC connection.
    assert(!trigger.SetDtr(true));
    trigger.SetBitRate(1200);
    assert(trigger.SetDtr(false));
}
