#include "watcher_io_expander.h"

#include <esp_log.h>

namespace {
constexpr char kTag[] = "WatcherIoExp";
constexpr uint8_t kRegInput0 = 0x00;
constexpr uint8_t kRegOutput0 = 0x02;
constexpr uint8_t kRegConfig0 = 0x06;
constexpr int kI2cTimeoutMs = 100;
}  // namespace

bool WatcherIoExpander::Initialize(i2c_master_bus_handle_t bus, uint8_t address,
                                   uint16_t input_mask, uint16_t output_mask) {
    if (bus == nullptr) return false;
    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = 400 * 1000,
    };
    if (i2c_master_bus_add_device(bus, &config, &device_) != ESP_OK) {
        device_ = nullptr;
        ESP_LOGE(kTag, "Cannot attach PCA9555 at 0x%02x", address);
        return false;
    }
    // 1 = input, so the exact input mask is the configuration value.
    if (!WriteRegister16(kRegConfig0, input_mask)) return false;
    outputs_ = 0;
    if (!WriteRegister16(kRegOutput0, outputs_ & output_mask)) return false;
    if (!ReadInputs(&inputs_)) return false;
    ESP_LOGI(kTag, "PCA9555 ready: inputs=0x%04x outputs=0x%04x",
             static_cast<unsigned>(input_mask),
             static_cast<unsigned>(output_mask));
    return true;
}

bool WatcherIoExpander::WriteOutputs(uint16_t levels) {
    if (device_ == nullptr) return false;
    outputs_ = levels;
    return WriteRegister16(kRegOutput0, outputs_);
}

bool WatcherIoExpander::SetOutputs(uint16_t pin_mask, bool level) {
    if (device_ == nullptr) return false;
    const uint16_t updated = level ? (outputs_ | pin_mask)
                                   : (outputs_ & static_cast<uint16_t>(~pin_mask));
    if (updated == outputs_) return true;
    outputs_ = updated;
    return WriteRegister16(kRegOutput0, outputs_);
}

bool WatcherIoExpander::ReadInputs(uint16_t* levels) {
    if (levels == nullptr || device_ == nullptr) return false;
    uint16_t value = 0;
    if (!ReadRegister16(kRegInput0, &value)) return false;
    inputs_ = value;
    if (levels != nullptr) *levels = value;
    return true;
}

bool WatcherIoExpander::InputLevel(uint16_t pin_mask, bool* level) {
    if (level == nullptr || device_ == nullptr) return false;
    if (!ReadInputs(&inputs_)) return false;
    *level = (inputs_ & pin_mask) != 0;
    return true;
}

bool WatcherIoExpander::WriteRegister16(uint8_t reg, uint16_t value) {
    if (device_ == nullptr) return false;
    const uint8_t payload[3] = {
        reg,
        static_cast<uint8_t>(value & 0xff),
        static_cast<uint8_t>((value >> 8) & 0xff),
    };
    const esp_err_t result =
        i2c_master_transmit(device_, payload, sizeof(payload), kI2cTimeoutMs);
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "Write reg 0x%02x failed: %s", reg,
                 esp_err_to_name(result));
        return false;
    }
    return true;
}

bool WatcherIoExpander::ReadRegister16(uint8_t reg, uint16_t* value) {
    if (device_ == nullptr || value == nullptr) return false;
    uint8_t data[2] = {0, 0};
    const esp_err_t result = i2c_master_transmit_receive(
        device_, &reg, 1, data, sizeof(data), kI2cTimeoutMs);
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "Read reg 0x%02x failed: %s", reg,
                 esp_err_to_name(result));
        return false;
    }
    *value = static_cast<uint16_t>(data[0]) |
             (static_cast<uint16_t>(data[1]) << 8);
    return true;
}
