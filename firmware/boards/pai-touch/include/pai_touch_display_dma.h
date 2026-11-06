// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <driver/spi_master.h>
#include <cstdint>

// Board startup brackets the LCD IO constructor to bind its SPI device.
void PaiTouchDisplayDmaBeginBinding(spi_host_device_t host);
void PaiTouchDisplayDmaEndBinding();
uint32_t PaiTouchDisplayDmaFaults();
bool PaiTouchDisplayDmaUsesStaging();
#if PXA_PAI_DISPLAY_DMA_TEST_FAULTS > 0
void PaiTouchDisplayDmaArmCameraTest();
#endif
