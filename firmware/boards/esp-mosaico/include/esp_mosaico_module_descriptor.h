// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <pxa_ext/peripherals.h>
#include <cstddef>
#include <cstdint>
#include <cstring>

// EEPROM V1 wire format from esp-mosaico-bsp 7cbfb390638dcb93522fc3cde094b2d43cf907c2.
// CRC-16/MODBUS independently protects descriptor, manufacturing and parameters.
namespace mosaico_board {
inline uint16_t ModuleWord(const uint8_t* p) { return p[0] | (uint16_t(p[1]) << 8); }
inline uint16_t ModuleCrc(const uint8_t* data, size_t size) {
    uint16_t crc = 0xffff;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0xa001 : 0);
    }
    return crc;
}
inline bool DecodeModule(const uint8_t* raw, size_t size, pxa_peripheral_module_t* output) {
    if (!raw || !output || size != 0x86 || std::memcmp(raw, "ESP", 3) ||
        ModuleWord(raw+0x34) != ModuleCrc(raw, 0x34) ||
        ModuleWord(raw+0x3e) != ModuleCrc(raw+0x36, 8) ||
        ModuleWord(raw+0x84) != ModuleCrc(raw+0x40, 0x44) || ModuleWord(raw+0x42) > 64)
        return false;
    *output = {};
    output->flags = 3;
    output->board_type = raw[3]; output->board_id = ModuleWord(raw+4);
    output->hardware_version = ModuleWord(raw+6); output->vendor_id = ModuleWord(raw+0x0a);
    output->serial_number = uint32_t(ModuleWord(raw+0x10)) | (uint32_t(ModuleWord(raw+0x12)) << 16);
    // Module strings are untrusted EEPROM bytes. Return bounded display text.
    for (size_t i = 0; i < sizeof(output->name); ++i) {
        const auto ch = raw[0x14+i];
        output->name[i] = ch >= 0x20 && ch < 0x7f ? char(ch) : (ch == 0 ? 0 : '?');
    }
    output->name[sizeof(output->name)-1] = 0;
    return true;
}
}
