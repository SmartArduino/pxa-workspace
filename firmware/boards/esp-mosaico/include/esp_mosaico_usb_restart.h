// SPDX-License-Identifier: MIT
#pragma once

// Call after shutdown notification, display DMA quiescence and USB teardown.
[[noreturn]] void EspMosaicoRestartToUsbDownload();
