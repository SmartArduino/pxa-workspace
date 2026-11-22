// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <driver/spi_master.h>
#include <cstddef>
#include <cstdint>

#ifndef PXA_PAI_LCD_TRANSFER_LINES
#define PXA_PAI_LCD_TRANSFER_LINES 4
#endif
static_assert(PXA_PAI_LCD_TRANSFER_LINES == 4 || PXA_PAI_LCD_TRANSFER_LINES == 8);

// Each chunk fits one DMA descriptor, and its size is a multiple of the
// PSRAM cache-line alignment.
inline constexpr std::size_t kPaiLcdTransferBytes =
    PXA_PAI_LCD_TRANSFER_LINES * 240 * sizeof(uint16_t);

// Board startup brackets the LCD IO constructor to bind its SPI device.
void PaiTouchDisplayDmaBeginBinding(spi_host_device_t host);
void PaiTouchDisplayDmaEndBinding();
uint32_t PaiTouchDisplayDmaFaults();
bool PaiTouchDisplayDmaUsesStaging();
#if PXA_PAI_DISPLAY_DMA_TEST_FAULTS > 0
void PaiTouchDisplayDmaArmCameraTest();
#endif
