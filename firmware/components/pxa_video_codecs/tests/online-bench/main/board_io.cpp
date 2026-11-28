// SPDX-License-Identifier: Apache-2.0
#include "board_io.hpp"
#include "esp_mosaico_config.h"
#include "esp_mosaico_usb_download.h"
#include "esp_mosaico_usb_restart.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <driver/i2s_std.h>
#include <driver/spi_master.h>
#include <esp_check.h>
#include <esp_codec_dev.h>
#include <esp_codec_dev_defaults.h>
#include <esp_efuse.h>
#include <esp_efuse_table.h>
#include <esp_heap_caps.h>
#include <esp_lcd_co5300.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_mac.h>
#include <es8311_codec.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <hal/usb_utmi_ll.h>
#include <tinyusb.h>
#include <tinyusb_cdc_acm.h>
#include <tinyusb_console.h>
#include <tinyusb_default_config.h>

namespace board_io {
namespace {
using namespace mosaico_board;
constexpr char tag[] = "native_board";
constexpr int volume = 45;
TaskHandle_t download_task;
UsbDownloadTrigger download_trigger;
esp_lcd_panel_handle_t panel;
SemaphoreHandle_t lcd_done, lcd_te;
i2c_master_bus_handle_t i2c;
i2s_chan_handle_t audio_tx;
esp_codec_dev_handle_t speaker;

esp_err_t output(int pin, int level, gpio_mode_t mode = GPIO_MODE_OUTPUT) {
    const auto gpio = static_cast<gpio_num_t>(pin);
    ESP_RETURN_ON_ERROR(gpio_set_level(gpio, level), tag, "pin level");
    ESP_RETURN_ON_ERROR(gpio_set_direction(gpio, mode), tag, "pin direction");
    return gpio_hold_dis(gpio);
}

void restart(void*) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    gpio_set_level(static_cast<gpio_num_t>(kAudioAmplifierEnable), 0);
    gpio_set_level(static_cast<gpio_num_t>(kPeripheralPower), 1);
    vTaskDelay(pdMS_TO_TICKS(100));
    tinyusb_console_deinit(TINYUSB_CDC_ACM_0);
    tinyusb_driver_uninstall();
    vTaskDelay(pdMS_TO_TICKS(100));
    usb_utmi_ll_enable_bus_clock(true);
    usb_utmi_ll_reset_register();
    EspMosaicoRestartToUsbDownload();
}

bool panel_done(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void*) {
    BaseType_t wake = pdFALSE;
    xSemaphoreGiveFromISR(lcd_done, &wake);
    return wake == pdTRUE;
}

void panel_te(void*) {
    BaseType_t wake = pdFALSE;
    xSemaphoreGiveFromISR(lcd_te, &wake);
    if (wake) portYIELD_FROM_ISR();
}

esp_err_t init_display(const HardwareConfig& hw) {
    // Same register sequence, 48 MHz QSPI and PSRAM DMA as the PXA board.
    static constexpr uint8_t page20[] = {0x20}, reg19[] = {0x10}, reg1c[] = {0xa0};
    static constexpr uint8_t page00[] = {0x00}, regc4[] = {0x80}, rgb565[] = {0x55};
    static constexpr uint8_t te[] = {0x00}, control[] = {0x20}, brightness[] = {0xff};
    static constexpr uint8_t range[] = {0x00, 0x00, 0x01, 0xdf};
    static const co5300_lcd_init_cmd_t commands[] = {
        {0x11, nullptr, 0, 120}, {0xfe, page20, 1, 0}, {0x19, reg19, 1, 0},
        {0x1c, reg1c, 1, 0}, {0xfe, page00, 1, 0}, {0xc4, regc4, 1, 0},
        {0x3a, rgb565, 1, 0}, {0x35, te, 1, 0}, {0x53, control, 1, 0},
        {0x51, brightness, 1, 0}, {0x63, brightness, 1, 0}, {0x2a, range, 4, 0},
        {0x2b, range, 4, 0},
    };
    spi_bus_config_t bus{};
    bus.data0_io_num = kDisplayData0;
    bus.data1_io_num = kDisplayData1;
    bus.data2_io_num = kDisplayData2;
    bus.data3_io_num = kDisplayData3;
    bus.data4_io_num = bus.data5_io_num = bus.data6_io_num = bus.data7_io_num = -1;
    bus.sclk_io_num = hw.display_clock;
    bus.max_transfer_sz = kWidth * kTransferRows * sizeof(uint16_t);
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO), tag, "QSPI");
    for (const int pin : {hw.display_clock, kDisplayChipSelect, kDisplayData0,
                          kDisplayData1, kDisplayData2, kDisplayData3}) {
        ESP_RETURN_ON_ERROR(gpio_set_drive_capability(static_cast<gpio_num_t>(pin), GPIO_DRIVE_CAP_3),
                            tag, "QSPI drive");
    }

    lcd_done = xSemaphoreCreateBinary();
    lcd_te = xSemaphoreCreateBinary();
    if (!lcd_done || !lcd_te) return ESP_ERR_NO_MEM;
    esp_lcd_panel_io_spi_config_t io_config{};
    io_config.cs_gpio_num = static_cast<gpio_num_t>(kDisplayChipSelect);
    io_config.dc_gpio_num = GPIO_NUM_NC;
    io_config.pclk_hz = kPixelClockHz;
    io_config.trans_queue_depth = 10;
    io_config.on_color_trans_done = panel_done;
    io_config.lcd_cmd_bits = 32;
    io_config.lcd_param_bits = 8;
    io_config.flags.quad_mode = true;
    io_config.flags.psram_dma_direct = true;
    esp_lcd_panel_io_handle_t io;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &io), tag, "LCD IO");
    co5300_vendor_config_t vendor{};
    vendor.init_cmds = commands;
    vendor.init_cmds_size = sizeof(commands) / sizeof(commands[0]);
    vendor.flags.use_qspi_interface = true;
    esp_lcd_panel_dev_config_t config{};
    config.reset_gpio_num = static_cast<gpio_num_t>(hw.display_reset);
    config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    config.bits_per_pixel = 16;
    config.vendor_config = &vendor;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_co5300(io, &config, &panel), tag, "CO5300");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), tag, "LCD reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), tag, "LCD init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_co5300_set_brightness(panel, 65), tag, "brightness");

    gpio_config_t te_config{};
    te_config.pin_bit_mask = 1ULL << kDisplayTe;
    te_config.mode = GPIO_MODE_INPUT;
    te_config.intr_type = GPIO_INTR_POSEDGE;
    ESP_RETURN_ON_ERROR(gpio_config(&te_config), tag, "LCD TE pin");
    ESP_RETURN_ON_ERROR(gpio_install_isr_service(0), tag, "GPIO ISR");
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(static_cast<gpio_num_t>(kDisplayTe), panel_te, nullptr),
                        tag, "LCD TE ISR");
    return esp_lcd_panel_disp_on_off(panel, true);
}
}  // namespace

esp_err_t init_usb() {
    static char serial[18];
    static const char language[] = {0x09, 0x04};
    static const char* strings[] = {language, "Espressif Systems", "ESP-Mosaico", serial, "Native video benchmark"};
    uint8_t mac[6];
    ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_WIFI_STA), tag, "MAC");
    std::snprintf(serial, sizeof(serial), "%02x:%02x:%02x:%02x:%02x:%02x",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    if (xTaskCreate(restart, "usb_boot", 3072, nullptr, 4, &download_task) != pdPASS)
        return ESP_ERR_NO_MEM;
    auto usb = TINYUSB_DEFAULT_CONFIG();
    usb.task.xCoreID = 0;
    usb.descriptor.string = strings;
    usb.descriptor.string_count = sizeof(strings) / sizeof(strings[0]);
    ESP_RETURN_ON_ERROR(tinyusb_driver_install(&usb), tag, "USB");
    tinyusb_config_cdcacm_t cdc{};
    cdc.cdc_port = TINYUSB_CDC_ACM_0;
    cdc.callback_line_coding_changed = [](int, cdcacm_event_t* event) {
        download_trigger.SetBitRate(event->line_coding_changed_data.p_line_coding->bit_rate);
    };
    cdc.callback_line_state_changed = [](int, cdcacm_event_t* event) {
        if (download_trigger.SetDtr(event->line_state_changed_data.dtr)) xTaskNotifyGive(download_task);
    };
    ESP_RETURN_ON_ERROR(tinyusb_cdcacm_init(&cdc), tag, "USB CDC");
    ESP_RETURN_ON_ERROR(tinyusb_console_init(TINYUSB_CDC_ACM_0), tag, "USB console");
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    return ESP_OK;
}

void report(const char* format, ...) {
    char line[470];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    std::fputs(line, stdout);
}

void read_wifi(wifi_config_t& config) {
    char line[sizeof(config.sta.ssid) + sizeof(config.sta.password) + 1]{};
    size_t used = 0;
    bool overflow = false;
    while (true) {
        uint8_t byte;
        size_t received = 0;
        if (tinyusb_cdcacm_read(TINYUSB_CDC_ACM_0, &byte, 1, &received) != ESP_OK || !received) {
            vTaskDelay(pdMS_TO_TICKS(15));
            continue;
        }
        if (byte != '\n') {
            if (used < sizeof(line) - 1) line[used++] = char(byte);
            else overflow = true;
            continue;
        }
        const char* tab = static_cast<const char*>(std::memchr(line, '\t', used));
        if (!overflow && tab && tab != line && size_t(tab - line) <= sizeof(config.sta.ssid) &&
            used - (tab - line) - 1 < sizeof(config.sta.password)) {
            std::memcpy(config.sta.ssid, line, tab - line);
            std::memcpy(config.sta.password, tab + 1, used - (tab - line) - 1);
            std::memset(line, 0, sizeof(line));
            return;
        }
        std::memset(line, 0, sizeof(line));
        overflow = false;
        used = 0;
        report("NATIVE credentials_invalid\n");
    }
}

esp_err_t init_board() {
    uint16_t revision = 0;
    ESP_RETURN_ON_ERROR(esp_efuse_read_field_blob(ESP_EFUSE_USER_DATA, &revision, sizeof(revision) * 8),
                        tag, "board revision");
    const auto hw = DecodeHardwareConfig(revision);
    if (!hw) return ESP_ERR_NOT_SUPPORTED;
    ESP_RETURN_ON_ERROR(output(kPowerSwitch, 1, GPIO_MODE_OUTPUT_OD), tag, "power latch");
    ESP_RETURN_ON_ERROR(output(kPeripheralPower, 0), tag, "peripheral rail");
    if (hw->codec_power >= 0) {
        ESP_RETURN_ON_ERROR(output(hw->codec_power, 1), tag, "codec rail");
    }
    vTaskDelay(pdMS_TO_TICKS(20));
    i2c_master_bus_config_t bus{};
    bus.i2c_port = I2C_NUM_0;
    bus.sda_io_num = static_cast<gpio_num_t>(hw->i2c_data);
    bus.scl_io_num = static_cast<gpio_num_t>(hw->i2c_clock);
    bus.clk_source = I2C_CLK_SRC_DEFAULT;
    bus.glitch_ignore_cnt = 7;
    bus.flags.enable_internal_pullup = true;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus, &i2c), tag, "audio I2C");
    report("NATIVE board=%s\n", hw->name);
    ESP_RETURN_ON_ERROR(init_display(*hw), tag, "LCD");
    report("NATIVE lcd_initialized\n");
    auto* black = static_cast<uint16_t*>(heap_caps_aligned_calloc(
        128, size_t(kWidth) * kHeight, sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!black) return ESP_ERR_NO_MEM;
    const auto result = draw_rgb565(black, kWidth, kHeight);
    if (result == ESP_OK) std::free(black);
    return result;
}

esp_err_t init_audio(unsigned rate) {
    ESP_RETURN_ON_ERROR(output(kAudioAmplifierEnable, 0), tag, "amplifier");
    i2s_chan_config_t channel{};
    channel.id = I2S_NUM_0;
    channel.role = I2S_ROLE_MASTER;
    channel.dma_desc_num = 6;
    channel.dma_frame_num = 512;
    channel.auto_clear = true;
#if CONFIG_ESP_OPENH264_PROFILE_PERFORMANCE
    // The Performance image leaves less DMA-capable SRAM after Wi-Fi starts.
    channel.dma_buffer_in_psram = true;
#endif
    ESP_RETURN_ON_ERROR(i2s_new_channel(&channel, &audio_tx, nullptr), tag, "I2S channel");
    const i2s_std_config_t std_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = static_cast<gpio_num_t>(kAudioMasterClock),
            .bclk = static_cast<gpio_num_t>(kAudioBitClock),
            .ws = static_cast<gpio_num_t>(kAudioWordSelect),
            .dout = static_cast<gpio_num_t>(kAudioDataOut),
            .din = GPIO_NUM_NC,
            .invert_flags = {},
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(audio_tx, &std_config), tag, "I2S format");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(audio_tx), tag, "I2S enable");
    audio_codec_i2s_cfg_t data{};
    data.port = I2S_NUM_0;
    data.tx_handle = audio_tx;
    audio_codec_i2c_cfg_t control = {.port = I2C_NUM_0, .addr = kAudioCodecAddress << 1, .bus_handle = i2c};
    es8311_codec_cfg_t codec{};
    codec.ctrl_if = audio_codec_new_i2c_ctrl(&control);
    codec.gpio_if = audio_codec_new_gpio();
    codec.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;
    codec.pa_pin = GPIO_NUM_NC;
    codec.use_mclk = true;
    codec.hw_gain.pa_voltage = 5.0f;
    codec.hw_gain.codec_dac_voltage = 3.3f;
    codec.no_dac_ref = true;
    codec.mclk_div = 256;
    if (!codec.ctrl_if || !codec.gpio_if) return ESP_ERR_NO_MEM;
    esp_codec_dev_cfg_t dev = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = es8311_codec_new(&codec),
        .data_if = audio_codec_new_i2s_data(&data),
    };
    if (!dev.codec_if || !dev.data_if || !(speaker = esp_codec_dev_new(&dev))) return ESP_ERR_NO_MEM;
    esp_codec_dev_sample_info_t info = {
        .bits_per_sample = 16, .channel = 2, .channel_mask = 0, .sample_rate = rate, .mclk_multiple = 256};
    if (esp_codec_dev_open(speaker, &info) != ESP_CODEC_DEV_OK ||
        esp_codec_dev_set_out_vol(speaker, volume) != ESP_CODEC_DEV_OK ||
        esp_codec_dev_set_out_mute(speaker, false) != ESP_CODEC_DEV_OK) return ESP_FAIL;
    vTaskDelay(pdMS_TO_TICKS(24));
    return gpio_set_level(static_cast<gpio_num_t>(kAudioAmplifierEnable), 1);
}

esp_err_t write_pcm(const uint8_t* pcm, size_t bytes) {
    size_t written = 0;
    const auto result = i2s_channel_write(audio_tx, pcm, bytes, &written, 250);
    return result == ESP_OK && written != bytes ? ESP_ERR_INVALID_SIZE : result;
}

void stop_audio() {
    // Allow the complete final PCM block to leave the I2S DMA ring.
    vTaskDelay(pdMS_TO_TICKS(80));
    gpio_set_level(static_cast<gpio_num_t>(kAudioAmplifierEnable), 0);
    if (speaker) esp_codec_dev_close(speaker);
}

esp_err_t draw_rgb565(const uint16_t* pixels, unsigned width, unsigned height) {
    const int x = (kWidth - width) / 2;
    const int y = (kHeight - height) / 2;
    xSemaphoreTake(lcd_te, 0);
    xSemaphoreTake(lcd_te, pdMS_TO_TICKS(24));
    ESP_RETURN_ON_ERROR(esp_lcd_panel_draw_bitmap(panel, x, y, x + width, y + height, pixels),
                        tag, "LCD draw");
    return xSemaphoreTake(lcd_done, pdMS_TO_TICKS(400)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}
}  // namespace board_io
