// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstddef>
#include <cstdint>
#include <numeric>
namespace espp4_a {
struct PpaRows {
    unsigned prefix = 0, rows = 0, tail = 0;
};
// Plan whole cache lines inside a tightly packed RGB565 rectangle. A row may
// be shorter than a cache line multiple (480 pixels occupy 960 bytes), so use
// an aligned group of rows rather than requiring each row to be aligned.
inline bool PlanPpaRows(uintptr_t address, unsigned width, unsigned height, size_t stride, size_t line,
                        PpaRows& output) {
    output = {};
    if (!address || width < 32 || width > 4095 || !height || height > 4095 || stride != size_t(width) * 2 ||
        address > UINTPTR_MAX - size_t(height) * stride || !line || (line & (line - 1)))
        return false;
    const size_t group = line / std::gcd(stride, line);
    unsigned prefix = 0;
    while (prefix < height && prefix < group && (address + size_t(prefix) * stride) % line)
        ++prefix;
    if (prefix == height || (address + size_t(prefix) * stride) % line)
        return false;
    const unsigned rows = unsigned((height - prefix) / group * group);
    if (uint64_t(rows) * width < 65536)
        return false;
    output = {prefix, rows, height - prefix - rows};
    return true;
}
} // namespace espp4_a
