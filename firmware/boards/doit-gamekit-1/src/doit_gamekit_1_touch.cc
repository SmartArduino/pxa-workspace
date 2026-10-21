#include "doit_gamekit_1_touch.h"
#include <driver/gpio.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace {
constexpr char kTag[] = "gamekit1_touch";
constexpr uint8_t kAddress = 0x38;
}

void DoitGamekit1Touch::Cleanup() {
    if (device_) { (void)i2c_master_bus_rm_device(device_); device_ = nullptr; }
    if (bus_) { (void)i2c_del_master_bus(bus_); bus_ = nullptr; }
}

bool DoitGamekit1Touch::Initialize() {
    using namespace doit_gamekit_1;
    const gpio_config_t reset = {
        .pin_bit_mask = 1ULL << kTouchReset, .mode = GPIO_MODE_OUTPUT,
    };
    const gpio_config_t interrupt = {
        .pin_bit_mask = 1ULL << kTouchInterrupt, .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    if (gpio_config(&reset) != ESP_OK || gpio_config(&interrupt) != ESP_OK) return false;
    gpio_set_level(gpio_num_t(kTouchReset), 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(gpio_num_t(kTouchReset), 1);
    vTaskDelay(pdMS_TO_TICKS(300)); // FT6336U datasheet Trsi, not an I2C retry
    const i2c_master_bus_config_t config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = gpio_num_t(kTouchSda), .scl_io_num = gpio_num_t(kTouchScl),
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
        .flags = {.enable_internal_pullup = true},
    };
    if (i2c_new_master_bus(&config, &bus_) != ESP_OK) return false;
    const i2c_device_config_t device = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = kAddress,
        .scl_speed_hz = 100000,
    };
    if (i2c_master_bus_add_device(bus_, &device, &device_) != ESP_OK ||
        i2c_master_probe(bus_, kAddress, 30) != ESP_OK) {
        Cleanup(); return false;
    }
    // Match game_-console: reset and read reports without changing mode registers.
    uint8_t id = 0, firmware = 0;
    const uint8_t id_reg = 0xa3, firmware_reg = 0xa6;
    if (i2c_master_transmit_receive(device_, &id_reg, 1, &id, 1, 30) != ESP_OK ||
        i2c_master_transmit_receive(device_, &firmware_reg, 1, &firmware, 1, 30) != ESP_OK) {
        Cleanup(); return false;
    }
    ESP_LOGI(kTag, "FT6336U ready: address=0x38 chip=0x%02x firmware=0x%02x, "
             "SDA=%d SCL=%d INT=%d RST=%d, 100 kHz, two contact slots",
             id, firmware, kTouchSda, kTouchScl, kTouchInterrupt, kTouchReset);
    return true;
}

bool DoitGamekit1Touch::Read(doit_gamekit_1::LandscapeRotation rotation,
                            doit_gamekit_1::TouchReport& report) {
    uint8_t bytes[13]{};
    const uint8_t status_reg = 0x02;
    return device_ && i2c_master_transmit_receive(device_, &status_reg, 1,
               bytes, sizeof(bytes), 20) == ESP_OK &&
        doit_gamekit_1::DecodeTouchReport(bytes, sizeof(bytes), rotation, report);
}
