#pragma once

#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <driver/i2s_std.h>
#include <driver/ledc.h>

// General I2C bus: PCA9555 IO expander, ES8311 speaker codec and the PCF8563
// RTC (unused by the PXA system).
#define WATCHER_I2C_PORT I2C_NUM_0
#define WATCHER_I2C_SDA GPIO_NUM_47
#define WATCHER_I2C_SCL GPIO_NUM_48
#define WATCHER_I2C_CLOCK_HZ 400000

// Touch I2C bus: SPD2010 touch controller.
#define WATCHER_TOUCH_I2C_PORT I2C_NUM_1
#define WATCHER_TOUCH_SDA GPIO_NUM_39
#define WATCHER_TOUCH_SCL GPIO_NUM_38
#define WATCHER_TOUCH_CLOCK_HZ 400000

// PCA9555 16-bit IO expander (P0.0..P0.7 = bits 0..7, P1.0..P1.7 = bits 8..15).
#define WATCHER_IO_EXPANDER_ADDRESS 0x21
#define WATCHER_IO_INT GPIO_NUM_2

#define WATCHER_IO_PIN(index) (UINT16_C(1) << (index))
#define WATCHER_IO_CHRG_DET WATCHER_IO_PIN(0)   // P0.0, high while charging
#define WATCHER_IO_STDBY_DET WATCHER_IO_PIN(1)  // P0.1, low while standby
#define WATCHER_IO_VBUS_DET WATCHER_IO_PIN(2)   // P0.2, USB VBUS present
#define WATCHER_IO_KNOB_BTN WATCHER_IO_PIN(3)   // P0.3, wheel button
#define WATCHER_IO_SD_DET WATCHER_IO_PIN(4)     // P0.4, microSD card detect
#define WATCHER_IO_TOUCH_INT WATCHER_IO_PIN(5)  // P0.5, SPD2010 interrupt
#define WATCHER_IO_PWR_SDCARD WATCHER_IO_PIN(8) // P1.0
#define WATCHER_IO_PWR_LCD WATCHER_IO_PIN(9)    // P1.1
#define WATCHER_IO_PWR_SYSTEM WATCHER_IO_PIN(10)  // P1.2, power latch
#define WATCHER_IO_PWR_AI_CHIP WATCHER_IO_PIN(11) // P1.3, Himax HX6538
#define WATCHER_IO_PWR_CODEC_PA WATCHER_IO_PIN(12) // P1.4
#define WATCHER_IO_PWR_BAT_DET WATCHER_IO_PIN(13)  // P1.5, low when battery present
#define WATCHER_IO_PWR_GROVE WATCHER_IO_PIN(14)    // P1.6
#define WATCHER_IO_PWR_BAT_ADC WATCHER_IO_PIN(15)  // P1.7

#define WATCHER_IO_INPUT_MASK (UINT16_C(0x00FF) | WATCHER_IO_PWR_BAT_DET)
// Power rails brought up once the system rail is latched.
#define WATCHER_IO_POWER_UP_MASK                                             \
    (WATCHER_IO_PWR_SDCARD | WATCHER_IO_PWR_LCD | WATCHER_IO_PWR_SYSTEM |    \
     WATCHER_IO_PWR_AI_CHIP | WATCHER_IO_PWR_CODEC_PA |                      \
     WATCHER_IO_PWR_GROVE | WATCHER_IO_PWR_BAT_ADC)

// QSPI LCD bus (SPI3) for the 412x412 SPD2010 panel.
#define WATCHER_LCD_SPI_HOST SPI3_HOST
#define WATCHER_LCD_PCLK GPIO_NUM_7
#define WATCHER_LCD_DATA0 GPIO_NUM_9
#define WATCHER_LCD_DATA1 GPIO_NUM_1
#define WATCHER_LCD_DATA2 GPIO_NUM_14
#define WATCHER_LCD_DATA3 GPIO_NUM_13
#define WATCHER_LCD_CS GPIO_NUM_45
#define WATCHER_LCD_BACKLIGHT GPIO_NUM_8
// 40 MHz, the clock the official Watcher BSP uses. 80 MHz occasionally
// corrupted refreshed areas, which points at signal integrity on the panel
// flex for this board.
#define WATCHER_LCD_PIXEL_CLK_HZ (40 * 1000 * 1000)
#define WATCHER_LCD_BACKLIGHT_CHANNEL LEDC_CHANNEL_0
#define WATCHER_LCD_BACKLIGHT_TIMER LEDC_TIMER_0
#define WATCHER_LCD_BACKLIGHT_DUTY_RES LEDC_TIMER_10_BIT
#define WATCHER_LCD_BACKLIGHT_FREQ_HZ 5000

#define WATCHER_DISPLAY_WIDTH 412
#define WATCHER_DISPLAY_HEIGHT 412

// On-board WS2812 status LED. The board keeps it off; PXA apps may claim it
// through their own package later.
#define WATCHER_RGB_LED GPIO_NUM_40

// Audio: ES8311 speaker over I2S0, 16 kHz mono 16-bit.
#define WATCHER_AUDIO_I2S_PORT I2S_NUM_0
#define WATCHER_AUDIO_MCLK GPIO_NUM_10
#define WATCHER_AUDIO_SCLK GPIO_NUM_11
#define WATCHER_AUDIO_LRCK GPIO_NUM_12
#define WATCHER_AUDIO_DSIN GPIO_NUM_15
#define WATCHER_AUDIO_DOUT GPIO_NUM_16
#define WATCHER_AUDIO_SAMPLE_RATE 16000
#define WATCHER_AUDIO_CHANNELS 1
#define WATCHER_AUDIO_ES8311_ADDRESS 0x30

// Battery: ADC1 channel 2 (GPIO3) through a 62k/20k divider.
#define WATCHER_BATTERY_ADC_UNIT ADC_UNIT_1
#define WATCHER_BATTERY_ADC_CHANNEL ADC_CHANNEL_2
#define WATCHER_BATTERY_DIVIDER_GAIN (82.0f / 20.0f)

// Wheel: quadrature encoder GPIOs and the push button on the IO expander.
#define WATCHER_KNOB_A GPIO_NUM_41
#define WATCHER_KNOB_B GPIO_NUM_42

#define WATCHER_STATUS_TIMER_PERIOD_US (1000 * 1000)
#define WATCHER_POWER_KEY_LONG_PRESS_MS 2000
#define WATCHER_POWER_KEY_SHORT_PRESS_MS 200
