#pragma once

#include <cstdint>

#include <driver/i2c_master.h>

// Minimal PCA9555/PCA9535 16-bit IO expander driver for the SenseCAP Watcher.
// Only the register access needed by the board port is implemented; the pins
// use P0.0..P0.7 for bit 0..7 and P1.0..P1.7 for bit 8..15.
class WatcherIoExpander {
public:
    bool Initialize(i2c_master_bus_handle_t bus, uint8_t address,
                    uint16_t input_mask, uint16_t output_mask);

    // Replace the output latch for every output pin.
    bool WriteOutputs(uint16_t levels);
    // Read-modify-write the output latch of the masked pins.
    bool SetOutputs(uint16_t pin_mask, bool level);
    // Refresh the cached input port from the expander.
    bool ReadInputs(uint16_t* levels);
    // Refresh the cached inputs and return one pin level.
    bool InputLevel(uint16_t pin_mask, bool* level);

    uint16_t cached_inputs() const { return inputs_; }

private:
    bool WriteRegister16(uint8_t reg, uint16_t value);
    bool ReadRegister16(uint8_t reg, uint16_t* value);

    i2c_master_dev_handle_t device_ = nullptr;
    uint16_t outputs_ = 0;
    uint16_t inputs_ = 0;
};
