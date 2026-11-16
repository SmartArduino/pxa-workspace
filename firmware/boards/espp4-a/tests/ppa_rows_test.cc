// SPDX-License-Identifier: Apache-2.0
#include "espp4_a_ppa_rows.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <vector>
int main() {
    unsigned dma_cases = 0, fallback = 0;
    for (unsigned w : {32u, 96u, 256u, 479u, 480u, 512u, 640u})
        for (unsigned h : {1u, 138u, 139u, 140u, 854u})
            for (unsigned off : {0u, 2u, 64u, 126u}) {
                size_t bytes = (size_t(w) * h * 2 + 512 + 127) & ~size_t(127);
                auto* buffer = static_cast<uint16_t*>(aligned_alloc(128, bytes));
                assert(buffer);
                std::vector<uint16_t> expected(bytes / 2);
                for (size_t i = 0; i < bytes / 2; i++)
                    buffer[i] = expected[i] = uint16_t(i * 3511u + 0x364a);
                auto* start = reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(buffer) + 128 + off);
                espp4_a::PpaRows p;
                if (espp4_a::PlanPpaRows(uintptr_t(start), w, h, size_t(w) * 2, 128, p)) {
                    assert(p.prefix + p.rows + p.tail == h);
                    auto* dma = start + size_t(p.prefix) * w;
                    size_t dma_bytes = size_t(p.rows) * w * 2;
                    assert(uintptr_t(dma) % 128 == 0 && dma_bytes % 128 == 0);
                    assert(reinterpret_cast<uint8_t*>(dma) >= reinterpret_cast<uint8_t*>(start));
                    assert(reinterpret_cast<uint8_t*>(dma) + dma_bytes <=
                           reinterpret_cast<uint8_t*>(start) + size_t(w) * h * 2);
                    std::fill_n(dma, size_t(p.rows) * w, 0x64bc);
                    std::fill_n(start, size_t(p.prefix) * w, 0x64bc);
                    std::fill_n(start + size_t(p.prefix + p.rows) * w, size_t(p.tail) * w, 0x64bc);
                    ++dma_cases;
                } else {
                    std::fill_n(start, size_t(w) * h, 0x64bc);
                    ++fallback;
                }
                std::fill_n(expected.data() + 64 + off / 2, size_t(w) * h, 0x64bc);
                assert(std::equal(expected.begin(), expected.end(), buffer));
                free(buffer);
            }
    espp4_a::PpaRows p;
    assert(!espp4_a::PlanPpaRows(UINTPTR_MAX - 8, 480, 854, 960, 128, p));
    assert(!espp4_a::PlanPpaRows(0x1000, 480, 854, 1024, 128, p));
    assert(dma_cases > 10 && fallback > 10);
    std::printf("PASS: %u DMA spans and %u fallbacks; CPU edges, cacheline ownership, adjacent guards and "
                "address overflow\n",
                dma_cases, fallback);
}
